#pragma once

#include <MaterialXCore/Document.h>
#include <string>

class DocumentLoader
{
public:
    // Takes the stdlib from MDLGenerator so both share the same instance
    explicit DocumentLoader(MaterialX::DocumentPtr stdlib, bool clamp = false);

    MaterialX::DocumentPtr load(const std::string& filepath);
    MaterialX::DocumentPtr create_empty();
    MaterialX::DocumentPtr get_stdlib() const { return m_stdlib; }

private:
    MaterialX::DocumentPtr m_stdlib;
    bool m_clamp;
};