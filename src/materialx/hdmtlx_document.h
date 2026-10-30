#pragma once

// hdMtlx's document conversion for the material service, with its validation
// report captured instead of printed.
//
// HdMtlxCreateMtlxDocumentFromHdMaterialNetworkInterface validates the
// document it builds and posts MaterialX's findings as one TF_WARN
// ("Validation warnings for generated MaterialX file", one line per input).
// Assets authored for older MaterialX versions or by Houdini (inputs typed
// after their authored USD value or after the node feeding them, such as a
// color3 fractal3d into a vector3 add) produce hundreds of such lines,
// although DocumentLoader repairs them before code generation, and what it
// cannot repair it reports itself when it validates the final document
// ("Document validation issues"). The report is therefore dropped from the
// log and handed to the caller, which shows it as "mdl" debug output.
// Everything else hdMtlx posts (missing definitions, unsupported parameter
// types, errors) is re-posted unchanged, and a failed conversion keeps its
// validation report too.

#include <pxr/base/tf/diagnosticTrap.h>
#include <pxr/base/tf/warning.h>
#include <pxr/imaging/hd/materialNetworkInterface.h>
#include <pxr/imaging/hdMtlx/hdMtlx.h>

#include <MaterialXCore/Document.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace prism::materialx {

// The commentary hdMtlx starts its validation warning with.
inline constexpr std::string_view kHdMtlxValidationWarning = "Validation warnings for generated MaterialX file";

struct HdMtlxValidationReport {
    // MaterialX's findings, one per line, without hdMtlx's heading.
    std::string text;
    size_t issues = 0;
};

// HdMtlxCreateMtlxDocumentFromHdMaterialNetworkInterface with its validation
// warning moved into `report` (when the conversion succeeds).
inline MaterialX::DocumentPtr CreateHdMtlxDocument(
    PXR_NS::HdMaterialNetworkInterface* net, PXR_NS::TfToken const& terminalNodeName,
    PXR_NS::TfTokenVector const& terminalNodeConnectionNames, MaterialX::DocumentPtr const& library,
    PXR_NS::HdMtlxTexturePrimvarData* mxHdData, HdMtlxValidationReport* report)
{
    // The trap is per thread: concurrent conversions and other threads'
    // diagnostics are unaffected. Its destructor re-posts what is not erased.
    PXR_NS::TfDiagnosticTrap trap;
    MaterialX::DocumentPtr doc = PXR_NS::HdMtlxCreateMtlxDocumentFromHdMaterialNetworkInterface(
        net, terminalNodeName, terminalNodeConnectionNames, library, mxHdData);
    if (!doc) return doc;
    trap.EraseMatching([report](PXR_NS::TfWarning const& warning) {
        const std::string& commentary = warning.GetCommentary();
        if (!std::string_view(commentary).starts_with(kHdMtlxValidationWarning)) return false;
        if (report) {
            size_t begin = commentary.find('\n');
            while (begin != std::string::npos) {
                const size_t end = commentary.find('\n', begin + 1);
                const std::string line = commentary.substr(
                    begin + 1, end == std::string::npos ? std::string::npos : end - begin - 1);
                if (line.find_first_not_of(" \t\r") != std::string::npos) {
                    report->text += line + "\n";
                    ++report->issues;
                }
                begin = end;
            }
        }
        return true;
    });
    return doc;
}

} // namespace prism::materialx
