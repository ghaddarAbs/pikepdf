#include <sstream>

#include <qpdf/Constants.h>
#include <qpdf/Types.h>
#include <qpdf/DLL.h>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFObjGen.hh>
#include <qpdf/QPDFXRefEntry.hh>
#include <qpdf/PointerHolder.hh>
#include <qpdf/Buffer.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

extern "C" const char* qpdf_get_qpdf_version();

using namespace std::literals::string_literals;

namespace py = pybind11;

QPDFObjectHandle objecthandle_encode(py::handle obj);


template <typename T>
void kwargs_to_method(py::kwargs kwargs, const char* key, std::unique_ptr<QPDF> &q, void (QPDF::*callback)(T))
{
    try {
        if (kwargs.contains(key)) {
            auto v = kwargs[key].cast<T>();
            ((*q).*callback)(v); // <-- Cute
        }
    } catch (py::cast_error &e) {
        throw py::type_error(std::string(key) + ": unsupported argument type");
    }
}


/* Convert a Python object to a filesystem encoded path
 * Use Python's os.fspath() which accepts os.PathLike (str, bytes, pathlib.Path)
 * and returns bytes encoded in the filesystem encoding.
 * Cast to a string without transcoding.
 */
std::string fsencode_filename(py::object py_filename)
{
    auto fspath = py::module::import("pikepdf._cpphelpers").attr("fspath");
    std::string filename;

    try {
        auto py_encoded_filename = fspath(py_filename);
        filename = py_encoded_filename.cast<std::string>();
    } catch (py::cast_error &e) {
        throw py::type_error("expected pathlike object");
    }

    return filename;
}

auto open_pdf(py::args args, py::kwargs kwargs)
{
    auto q = std::make_unique<QPDF>();

    if (args.size() < 1) 
        throw py::value_error("not enough arguments");
    if (args.size() > 2)
        throw py::value_error("too many arguments");

    std::string password;

    q->setSuppressWarnings(true);
    if (kwargs) {
        if (kwargs.contains("password")) {
            auto v = kwargs["password"].cast<std::string>();
            password = v;
        }
        kwargs_to_method(kwargs, "ignore_xref_streams", q, &QPDF::setIgnoreXRefStreams);
        kwargs_to_method(kwargs, "suppress_warnings", q, &QPDF::setSuppressWarnings);
        kwargs_to_method(kwargs, "attempt_recovery", q, &QPDF::setAttemptRecovery);
    }

    if (py::hasattr(args[0], "read") && py::hasattr(args[0], "seek")) {
        // Python code gave us an object with a stream interface
        py::object stream = args[0];
        auto TextIOBase = py::module::import("io").attr("TextIOBase");

        if (py::isinstance(stream, TextIOBase)) {
            throw py::type_error("stream must be binary, readable and seekable");
        }

        py::object read = stream.attr("read");
        py::object pydata = read();  // i.e. self=stream
        py::bytes data = pydata.cast<py::bytes>();
        char *buffer;
        ssize_t length;

        PYBIND11_BYTES_AS_STRING_AND_SIZE(data.ptr(), &buffer, &length);

        // libqpdf will create a copy of this memory and attach it
        // to 'q'
        q->processMemoryFile("memory", buffer, length, password.c_str());
    } else {
        std::string filename = fsencode_filename(args[0]);
        py::gil_scoped_release release;
        q->processFile(filename.c_str(), password.c_str());
    }

    return q;
}


void init_object(py::module& m);

PYBIND11_MODULE(_qpdf, m) {
    //py::options options;
    //options.disable_function_signatures();

    m.doc() = "pikepdf provides a Pythonic interface for QPDF";

    m.def("qpdf_version", &qpdf_get_qpdf_version, "Get libqpdf version");

    static py::exception<QPDFExc> exc_main(m, "PDFError");
    static py::exception<QPDFExc> exc_password(m, "PasswordError");
    py::register_exception_translator([](std::exception_ptr p) {
        try {
            if (p) std::rethrow_exception(p);
        } catch (const QPDFExc &e) {
            if (e.getErrorCode() == qpdf_e_password) {
                exc_password(e.what());
            } else {
                exc_main(e.what());
            }
        }
    });

    py::class_<QPDF>(m, "PDF", "In-memory representation of a PDF")
        .def_static("new",
            []() {
                auto q = std::make_unique<QPDF>();
                q->emptyPDF();
                q->setSuppressWarnings(true);
                return q;
            },
            "create a new empty PDF from stratch"
        )
        .def_static("open", open_pdf,
            R"~~~(
            Open an existing file at `filename` according to `options`, all
            of which are optional.

            :param os.PathLike filename: Filename of PDF to open
            :param password: User or owner password to open the PDF, if encrypted
            :type password: str or None
            :param ignore_xref_streams: If True, ignore cross-reference streams. See qpdf documentation.
            :param suppress_warnings: If True (default), warnings are not printed to stderr. Use `get_warnings()` to retrieve warnings.
            :param attempt_recovery: If True (default), attempt to recover from PDF parsing errors.
            :throws pikepdf.PasswordError: If the password failed to open the file.
            :throws pikepdf.PDFError: If for other reasons we could not open the file.
            )~~~"
        )
        .def("__repr__",
            [](const QPDF &q) {
                return "<pikepdf.PDF description='"s + q.getFilename() + "'>"s;
            }
        )
        .def_property_readonly("filename", &QPDF::getFilename,
            "the source filename of an existing PDF, when available")
        .def_property_readonly("pdf_version", &QPDF::getPDFVersion,
            "the PDF standard version, such as '1.7'")
        .def_property_readonly("extension_level", &QPDF::getExtensionLevel)
        .def_property_readonly("root", &QPDF::getRoot,
            "the /Root object of the PDF")
        .def_property_readonly("trailer", &QPDF::getTrailer,
            "the PDF trailer")
        .def_property_readonly("pages", &QPDF::getAllPages)
        .def_property_readonly("is_encrypted", &QPDF::isEncrypted)
        .def("get_warnings", &QPDF::getWarnings)  // this is a def because it modifies state by clearing warnings
        .def("show_xref_table", &QPDF::showXRefTable)
        .def("add_page", &QPDF::addPage,
            R"~~~(
            Attach a page to this PDF. The page can be either be a
            newly constructed PDF object or it can be obtained from another
            PDF.

            :param pikepdf.Object page: The page object to attach
            :param bool first: If True, prepend this before the first page; if False append after last page
            )~~~",
            py::arg("page"),
            py::arg("first")
        )
        .def("remove_page", &QPDF::removePage)
        .def("save",
             [](QPDF &q, py::object filename, bool static_id=false,
                bool preserve_pdfa=false) {
                QPDFWriter w(q, fsencode_filename(filename).c_str());
                {
                    py::gil_scoped_release release;
                    if (static_id) {
                        w.setStaticID(true);
                        w.setStreamDataMode(qpdf_s_uncompress);
                    }
                    w.write();
                }
                if (preserve_pdfa) {
                    auto helpers = py::module::import("pikepdf._cpphelpers");
                    helpers.attr("repair_pdfa")(filename);
                }
             },
             "save as a PDF",
             py::arg("filename"),
             py::arg("static_id") = false,
             py::arg("preserve_pdfa") = false
        )
        .def("_get_object_id", &QPDF::getObjectByID)
        .def("make_indirect", &QPDF::makeIndirectObject)
        .def("make_indirect",
            [](QPDF &q, py::object obj) -> QPDFObjectHandle {
                return q.makeIndirectObject(objecthandle_encode(obj));
            }
        )
        .def("_replace_object",
            [](QPDF &q, int objid, int gen, QPDFObjectHandle &h) {
                q.replaceObject(objid, gen, h);
            }
        );

    init_object(m);

#ifdef VERSION_INFO
    m.attr("__version__") = py::str(VERSION_INFO);
#else
    m.attr("__version__") = py::str("dev");
#endif
}
