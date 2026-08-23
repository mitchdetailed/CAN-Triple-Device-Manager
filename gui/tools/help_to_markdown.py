"""Convert the compiled manual's HTML pages into GitHub-flavoured Markdown.

The help pages under help/pages/ are the manual's single source of truth: the
program compiles them into the offline Help (F1), and this script derives the
docs/ tree the PUBLIC repository shows on GitHub. One source, two renderings —
editing a help page updates both on the next release, and there is no second
manual to drift.

The conversion is deliberately conservative:

  * Headings, paragraphs, lists, code and emphasis become real Markdown.
  * TABLES pass through as raw HTML. GitHub renders inline HTML tables
    perfectly, and the manual's tables carry links, line breaks and nested
    markup that a pipe-table conversion would mangle. Fidelity beats purity.
  * <p class="note"> / <p class="warn"> become blockquotes with a bold label,
    the closest GitHub idiom to the help's tinted boxes.
  * The per-page licence footer is dropped — the repository's own licence file
    covers it once, and 24 repetitions of it are noise in a docs folder.
  * Links between pages are rewritten .html -> .md so navigation keeps
    working; everything else about an href is preserved.

The docs index (docs/README.md) is generated from the manual's own table of
contents in help/cantriple.qhp, so the GitHub docs and the offline Help agree
about structure by construction.

    python help_to_markdown.py <help_dir> <out_dir>
    e.g. python help_to_markdown.py ../help ../../public/docs
"""

import html
import html.parser
import os
import re
import sys
import xml.etree.ElementTree as ET


class PageConverter(html.parser.HTMLParser):
    """One page, HTML -> Markdown. Structural tags become Markdown; unknown
    or table markup passes through verbatim (with .html links rewritten)."""

    BLOCK_NOTE = {"note": "**Note:**", "warn": "**Warning:**"}

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.out = []            # completed blocks
        self.buf = []            # current inline run
        self.title = ""
        self.in_title = False
        self.skip_depth = 0      # inside a dropped element (footer)
        self.table_depth = 0     # inside a table: raw HTML passthrough
        self.list_stack = []     # "ul"/"ol" nesting, for indent + markers
        self.note_label = None   # current <p> is a note/warn
        self.pre_depth = 0
        self.code_depth = 0      # inside an inline <code> span

    # ------------------------------------------------------------ helpers

    @staticmethod
    def _rewrite_href(href):
        # Only intra-manual links: an EXTERNAL page that happens to end in
        # .html (gnu.org's licence pages do) must keep its real extension.
        if "://" in href:
            return href
        # index.html becomes overview.md (README.md is the generated TOC), so
        # in-page links to it must follow the rename.
        href = re.sub(r"^index\.html(#|$)", r"overview.md\1", href)
        return re.sub(r"\.html(#|$)", r".md\1", href)

    def _flush_block(self, prefix=""):
        text = "".join(self.buf).strip()
        self.buf = []
        if not text:
            return
        # Collapse the source's hard-wrapped lines; Markdown reflows anyway.
        text = re.sub(r"\s*\n\s*", " ", text)
        if prefix:
            text = prefix + " " + text
        self.out.append(text)

    def _flush_pre(self):
        # Verbatim, minus a leading/trailing blank line: collapsing the
        # newlines here once let a Lua comment swallow the code after it.
        text = "".join(self.buf)
        self.buf = []
        self.out.append(text.strip("\n"))

    def _raw_attrs(self, attrs):
        parts = []
        for k, v in attrs:
            if v is None:
                parts.append(k)
            else:
                if k == "href":
                    v = self._rewrite_href(v)
                parts.append('%s="%s"' % (k, html.escape(v, quote=True)))
        return (" " + " ".join(parts)) if parts else ""

    # ------------------------------------------------------------ handlers

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if self.skip_depth:
            self.skip_depth += 1
            return
        if tag == "title":
            self.in_title = True
            return
        if self.table_depth:
            self.buf.append("<%s%s>" % (tag, self._raw_attrs(attrs)))
            if tag == "table":
                self.table_depth += 1
            return
        if tag == "table":
            self._flush_block()
            self.table_depth = 1
            self.buf.append("<table>")
            return
        if tag in ("h1", "h2", "h3", "h4"):
            self._flush_block()
            # Headings with an id= are link TARGETS (href="#marking" and
            # cross-page anchors). GitHub slugs headings by their text, not
            # their lost id, so the id is re-emitted as an explicit HTML
            # anchor the links keep resolving against.
            if a.get("id"):
                self.out.append('<a id="%s"></a>' % a["id"])
            self.buf.append("#" * int(tag[1]) + " ")
            return
        if tag == "p":
            self._flush_block()
            cls = a.get("class", "")
            if cls == "footer":
                self.skip_depth = 1
                return
            self.note_label = self.BLOCK_NOTE.get(cls)
            return
        if tag in ("ul", "ol"):
            self._flush_block()
            self.list_stack.append([tag, 0])
            return
        if tag == "li":
            self._flush_block()
            if self.list_stack:
                kind, count = self.list_stack[-1]
                self.list_stack[-1][1] += 1
                # Four spaces per level, not two: a nested list under an
                # ORDERED item needs its content past the "5. " marker width
                # or CommonMark un-nests it into a new top-level list.
                indent = "    " * (len(self.list_stack) - 1)
                marker = "-" if kind == "ul" else "%d." % (count + 1)
                self.buf.append("\x00LI\x00" + indent + marker + " ")
            return
        if tag == "pre":
            self._flush_block()
            self.pre_depth += 1
            self.out.append("```")
            return
        if tag == "code":
            # Handled BEFORE the verbatim guard, or a <code> inside a <pre>
            # bumps the depth on the way in and the guard swallows the end
            # tag on the way out — after which every later code span in the
            # page loses its opening backtick. The backtick itself is only
            # emitted for a top-level span outside any fence.
            emit = not self.pre_depth and self.code_depth == 0
            self.code_depth += 1
            if emit:
                self.buf.append("`")
            return
        if self.pre_depth or self.code_depth:
            # No emphasis inside verbatim text: a <b> or <i> there becomes
            # literal asterisks inside a code span.
            return
        if tag in ("b", "strong"):
            self.buf.append("**")
            return
        if tag in ("i", "em"):
            self.buf.append("*")
            return
        if tag == "a":
            self.buf.append("[")
            self._a_href = self._rewrite_href(a.get("href", ""))
            return
        if tag == "br":
            self.buf.append("\n")
            return
        if tag == "sup":
            self.buf.append("^")
            return
        # head/body/html/meta/link and anything else: ignored.

    def handle_endtag(self, tag):
        if self.skip_depth:
            self.skip_depth -= 1
            return
        if tag == "title":
            self.in_title = False
            return
        if self.table_depth:
            self.buf.append("</%s>" % tag)
            if tag == "table":
                self.table_depth -= 1
                if self.table_depth == 0:
                    self.out.append("".join(self.buf))
                    self.buf = []
            return
        if tag in ("h1", "h2", "h3", "h4"):
            self._flush_block()
            return
        if tag == "p":
            label = self.note_label
            self.note_label = None
            if label:
                text = "".join(self.buf).strip()
                self.buf = []
                text = re.sub(r"\s*\n\s*", " ", text)
                if text:
                    self.out.append("> " + label + " " + text)
            else:
                self._flush_block()
            return
        if tag in ("ul", "ol"):
            self._flush_block()
            if self.list_stack:
                self.list_stack.pop()
            return
        if tag == "li":
            self._flush_block()
            return
        if tag == "pre":
            self.pre_depth -= 1
            self._flush_pre()
            self.out.append("```")
            return
        if tag == "code":
            # Mirror of the start handler: always rebalance the depth, and
            # only close a backtick that was actually opened (a span nested
            # in a fence emitted none).
            if self.code_depth:
                self.code_depth -= 1
            if not self.pre_depth and self.code_depth == 0:
                self.buf.append("`")
            return
        if self.pre_depth or self.code_depth:
            return
        if tag in ("b", "strong"):
            self.buf.append("**")
            return
        if tag in ("i", "em"):
            self.buf.append("*")
            return
        if tag == "a":
            self.buf.append("](%s)" % getattr(self, "_a_href", ""))
            return
        if tag == "sup":
            return

    def handle_data(self, data):
        if self.skip_depth:
            return
        if self.in_title:
            self.title += data
            return
        if self.table_depth:
            self.buf.append(html.escape(data, quote=False))
            return
        if self.pre_depth or self.code_depth:
            # Verbatim contexts: backslash escapes do not work inside code
            # spans, so the text goes through raw.
            self.buf.append(data)
            return
        # Prose. Characters Markdown or GitHub's HTML pass would swallow are
        # neutralised: a literal * or _ from the source must not open
        # emphasis (the manual's "(*)" once rendered as five asterisks), and
        # an unescaped <word> reads as an unknown HTML tag and disappears.
        data = data.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        data = data.replace("*", "\\*").replace("_", "\\_")
        self.buf.append(data)

    # ------------------------------------------------------------ result

    def markdown(self):
        self._flush_block()
        blocks = []
        for b in self.out:
            # List items carry a marker so consecutive items stay adjacent
            # (one blank line between blocks would break the list in GH).
            blocks.append(b.replace("\x00LI\x00", ""))
        text = "\n\n".join(blocks)
        # Re-join consecutive list items without blank lines between them.
        text = re.sub(r"\n\n(?=(?:    )*(?:-|\d+\.) )", "\n", text)
        # Code fences hug their content: the fence markers and the verbatim
        # text are separate blocks, and the blank join lines would otherwise
        # render as empty lines inside every code block.
        text = text.replace("```\n\n", "```\n").replace("\n\n```", "\n```")
        return text.strip() + "\n"


def convert_page(src_path):
    with open(src_path, "r", encoding="utf-8") as f:
        conv = PageConverter()
        conv.feed(f.read())
    return conv.markdown()


def build_index(qhp_path):
    """docs/README.md from the manual's own TOC, so GitHub and the offline
    Help cannot disagree about the manual's structure."""
    tree = ET.parse(qhp_path)
    toc = tree.getroot().find(".//toc")
    lines = [
        "# CAN Triple Device Manager — Manual",
        "",
        "The same manual the program shows offline (Help > Contents, F1),",
        "rendered for GitHub. Generated from the help sources by",
        "`gui/tools/help_to_markdown.py` — edit the help pages, not these files.",
        "",
        "New here? Start with [how the program is organised](overview.md),",
        "then the first entry below.",
        "",
    ]

    def ref_to_md(ref):
        return ref.replace("pages/", "").replace(".html", ".md")

    def walk(section, depth):
        title = section.get("title", "")
        ref = section.get("ref", "")
        if depth == 0:
            # The root section is the manual itself; its children are the TOC.
            for child in section.findall("section"):
                walk(child, depth + 1)
            return
        indent = "  " * (depth - 1)
        lines.append("%s- [%s](%s)" % (indent, title, ref_to_md(ref)))
        for child in section.findall("section"):
            walk(child, depth + 1)

    for section in toc.findall("section"):
        walk(section, 0)
    lines.append("")
    return "\n".join(lines)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    help_dir, out_dir = sys.argv[1], sys.argv[2]
    pages_dir = os.path.join(help_dir, "pages")
    os.makedirs(out_dir, exist_ok=True)

    count = 0
    for name in sorted(os.listdir(pages_dir)):
        if not name.endswith(".html"):
            continue
        md = convert_page(os.path.join(pages_dir, name))
        # index.html describes the app's menu layout, useful in the app's own
        # help but redundant beside the generated TOC index — still converted
        # (as overview.md) and linked from it.
        out_name = "overview.md" if name == "index.html" else name.replace(".html", ".md")
        with open(os.path.join(out_dir, out_name), "w", encoding="utf-8", newline="\n") as f:
            f.write(md)
        count += 1

    index = build_index(os.path.join(help_dir, "cantriple.qhp"))
    index = index.replace("(index.md)", "(overview.md)")
    with open(os.path.join(out_dir, "README.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write(index)
    print("converted %d pages -> %s" % (count, out_dir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
