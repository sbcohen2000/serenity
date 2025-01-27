/*
 * Copyright (c) 2019-2020, Sergey Bugaev <bugaevc@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibMarkdown/Block.h>
#include <LibMarkdown/Heading.h>
#include <LibMarkdown/LineIterator.h>
#include <LibMarkdown/Text.h>

namespace Markdown {

class CodeBlock final : public Block {
public:
    CodeBlock(DeprecatedString const& language, DeprecatedString const& style, DeprecatedString const& code, Heading* current_section)
        : m_code(move(code))
        , m_language(language)
        , m_style(style)
        , m_current_section(current_section)
    {
    }
    virtual ~CodeBlock() override = default;

    virtual DeprecatedString render_to_html(bool tight = false) const override;
    virtual DeprecatedString render_for_terminal(size_t view_width = 0) const override;
    virtual RecursionDecision walk(Visitor&) const override;
    static OwnPtr<CodeBlock> parse(LineIterator& lines, Heading* current_section);

private:
    DeprecatedString m_code;
    DeprecatedString m_language;
    DeprecatedString m_style;
    Heading* m_current_section;

    static OwnPtr<CodeBlock> parse_backticks(LineIterator& lines, Heading* current_section);
    static OwnPtr<CodeBlock> parse_indent(LineIterator& lines);
};

}
