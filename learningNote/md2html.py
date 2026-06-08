#!/usr/bin/env python3
"""
Convert all .md files in the learningNote directory to .html files.
Images are styled to display fully (responsive, no clipping).
"""

import re
import os
import sys
from html import escape

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

CSS_STYLE = """
<style>
/* ================================================================
   Claude-style Design — warm, elegant, readable
   ================================================================ */

/* ========== Global ========== */
:root {
    --bg:            #FAF7F2;
    --bg-card:       #F5F0E8;
    --bg-code:       #F0EBE0;
    --bg-code-block: #F5F0E8;
    --text:          #2D2A26;
    --text-muted:    #6B625A;
    --border:        #E6DFD3;
    --border-light:  #F0EBE3;
    --accent:        #D9750A;
    --accent-hover:  #B85D08;
    --accent-soft:   #FDF0E0;
    --heading:       #1E1B18;
    --link:          #C2680A;
    --link-hover:    #924D06;
    --th-bg:         #F5F0E8;
    --th-text:       #5C5144;
    --quote-border:  #E8A84C;
    --quote-bg:      #FDF8F0;
}

body {
    font-family: "Georgia", "Noto Serif", "Times New Roman", serif;
    line-height: 1.8;
    max-width: 860px;
    margin: 0 auto;
    padding: 40px 24px;
    color: var(--text);
    background: var(--bg);
    -webkit-font-smoothing: antialiased;
    -moz-osx-font-smoothing: grayscale;
}

/* ========== Headings ========== */
h1, h2, h3, h4, h5, h6 {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto,
                 "Noto Sans", "Helvetica Neue", Arial, sans-serif;
    color: var(--heading);
    font-weight: 700;
    letter-spacing: -0.01em;
}
h1 {
    font-size: 2.1em;
    margin-top: 40px;
    margin-bottom: 16px;
    padding-bottom: 12px;
    border-bottom: 3px solid var(--accent);
}
h2 {
    font-size: 1.65em;
    margin-top: 36px;
    margin-bottom: 12px;
    padding-bottom: 8px;
    border-bottom: 1px solid var(--border);
    color: #3D3429;
}
h3 {
    font-size: 1.35em;
    margin-top: 30px;
    margin-bottom: 10px;
    color: #4A3F32;
}
h4 { font-size: 1.15em; margin-top: 24px; margin-bottom: 8px; color: #5C5144; }
h5, h6 { font-size: 1em; margin-top: 20px; margin-bottom: 6px; color: #6B625A; }

/* ========== Paragraphs ========== */
p { margin: 0 0 16px 0; }

/* ========== Links ========== */
a {
    color: var(--link);
    text-decoration: none;
    border-bottom: 1px solid transparent;
    transition: border-color 0.15s ease, color 0.15s ease;
}
a:hover {
    color: var(--link-hover);
    border-bottom-color: var(--link-hover);
}

/* ========== Code (inline) ========== */
code {
    font-family: "Cascadia Code", "Fira Code", "JetBrains Mono",
                 "SF Mono", Consolas, "Liberation Mono", monospace;
    font-size: 0.88em;
    background: var(--bg-code);
    color: #8B4513;
    padding: 2px 7px;
    border-radius: 4px;
    border: 1px solid var(--border-light);
}
pre {
    background: var(--bg-code-block);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 18px 20px;
    overflow-x: auto;
    line-height: 1.55;
    margin: 18px 0;
}
pre code {
    background: transparent;
    padding: 0;
    border: none;
    border-radius: 0;
    font-size: 0.85em;
    color: #3D3429;
}

/* ========== Blockquotes ========== */
blockquote {
    border-left: 4px solid var(--quote-border);
    margin: 18px 0;
    padding: 12px 20px;
    background: var(--quote-bg);
    border-radius: 0 6px 6px 0;
    color: #5C5144;
}
blockquote p { margin-bottom: 8px; }
blockquote p:last-child { margin-bottom: 0; }

/* ========== Tables — Claude-style warm ========== */
table {
    border-collapse: collapse;
    width: 100%;
    margin: 20px 0;
    font-size: 0.95em;
    overflow-x: auto;
    display: block;
}
thead { border-bottom: 2px solid var(--border); }
th {
    background: var(--th-bg);
    color: var(--th-text);
    font-weight: 700;
    padding: 10px 16px;
    text-align: left;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    font-size: 0.85em;
    text-transform: uppercase;
    letter-spacing: 0.03em;
}
td {
    padding: 9px 16px;
    border-bottom: 1px solid var(--border-light);
}
tr:last-child td { border-bottom: none; }
tr:nth-child(even) td { background: rgba(245, 240, 232, 0.4); }

/* ========== Images — fully displayed ========== */
img {
    max-width: 100%;
    height: auto;
    display: block;
    margin: 20px auto;
    border-radius: 6px;
    box-shadow: 0 1px 4px rgba(0,0,0,0.06);
}
img[src$=".svg"] {
    max-width: 100%;
    height: auto;
    min-width: 200px;
    background: #fff;
    border-radius: 6px;
}

/* ========== Lists ========== */
ul, ol { padding-left: 28px; margin: 12px 0; }
li { margin: 5px 0; }
li > p { margin: 0; }
ul li::marker { color: var(--accent); }
ol li::marker { color: var(--text-muted); font-weight: 600; }

/* ========== Horizontal rule ========== */
hr {
    border: none;
    border-top: 1px solid var(--border);
    margin: 36px 0;
}

/* ========== Task list ========== */
.task-list-item { list-style: none; margin-left: -24px; }
.task-list-item input { margin-right: 8px; accent-color: var(--accent); }

/* ========== Selection ========== */
::selection {
    background: #FDE8C8;
    color: #3D2B0F;
}
</style>
"""


def parse_inline(text):
    """Parse inline markdown: bold, italic, code, images, links, strikethrough."""
    # Images: ![alt](url)  -- must be processed before links
    text = re.sub(r'!\[([^\]]*)\]\(([^)\s]+(?:\s+"[^"]*")?)\)',
                  lambda m: f'<img src="{m.group(2)}" alt="{m.group(1)}">',
                  text)

    # Links
    text = re.sub(r'\[([^\]]*)\]\(([^)\s]+(?:\s+"[^"]*")?)\)',
                  lambda m: f'<a href="{m.group(2)}">{m.group(1)}</a>',
                  text)

    # Bold+Italic: ***text*** or ___text___
    text = re.sub(r'\*\*\*(.+?)\*\*\*', r'<strong><em>\1</em></strong>', text)
    text = re.sub(r'___(.+?)___', r'<strong><em>\1</em></strong>', text)

    # Bold: **text** or __text__
    text = re.sub(r'\*\*(.+?)\*\*', r'<strong>\1</strong>', text)
    text = re.sub(r'__(.+?)__', r'<strong>\1</strong>', text)

    # Italic: *text* or _text_
    text = re.sub(r'\*(.+?)\*', r'<em>\1</em>', text)
    text = re.sub(r'\b_(.+?)_\b', r'<em>\1</em>', text)

    # Strikethrough: ~~text~~
    text = re.sub(r'~~(.+?)~~', r'<del>\1</del>', text)

    # Inline code: `text`
    text = re.sub(r'`([^`]+)`', r'<code>\1</code>', text)

    return text


def convert_md_to_html(md_text, filename):
    """Convert a markdown string to a full HTML page."""
    lines = md_text.split('\n')
    html_lines = []
    i = 0
    n = len(lines)

    while i < n:
        line = lines[i]

        # --- Fenced code block ---
        if line.strip().startswith('```'):
            fence_lang = line.strip()[3:].strip()
            code_lines = []
            i += 1
            while i < n and not lines[i].strip().startswith('```'):
                code_lines.append(lines[i])
                i += 1
            i += 1  # skip closing ```
            code_text = '\n'.join(code_lines)
            # Escape HTML in code
            code_text = escape(code_text)
            if fence_lang:
                html_lines.append(f'<pre><code class="language-{fence_lang}">{code_text}</code></pre>')
            else:
                html_lines.append(f'<pre><code>{code_text}</code></pre>')
            continue

        # --- Empty line ---
        if line.strip() == '':
            html_lines.append('')
            i += 1
            continue

        # --- Horizontal rule ---
        if re.match(r'^[-*_]{3,}\s*$', line.strip()):
            html_lines.append('<hr>')
            i += 1
            continue

        # --- Header ---
        h_match = re.match(r'^(#{1,6})\s+(.+?)(?:\s+#+)?$', line)
        if h_match:
            level = len(h_match.group(1))
            content = parse_inline(h_match.group(2))
            html_lines.append(f'<h{level}>{content}</h{level}>')
            i += 1
            continue

        # --- Blockquote ---
        if line.startswith('>'):
            quote_lines = []
            while i < n and lines[i].startswith('>'):
                ql = lines[i]
                # Remove the leading '>' and optional space
                ql = re.sub(r'^>\s?', '', ql)
                quote_lines.append(ql)
                i += 1
            quote_text = '\n'.join(quote_lines)
            # Recursively process blockquote content
            quote_html = convert_inline_md(quote_text)
            html_lines.append(f'<blockquote>{quote_html}</blockquote>')
            continue

        # --- Unordered list ---
        ul_match = re.match(r'^(\s*)[-*+]\s+(.*)', line)
        if ul_match:
            items = []
            while i < n:
                m = re.match(r'^(\s*)[-*+]\s+(.*)', lines[i])
                if not m:
                    break
                indent = len(m.group(1))
                item_text = m.group(2)
                # Check for task list
                task_match = re.match(r'^\[([ xX])\]\s+(.*)', item_text)
                if task_match:
                    checked = 'checked' if task_match.group(1).lower() == 'x' else ''
                    item_text = task_match.group(2)
                    items.append(f'<li class="task-list-item"><input type="checkbox" disabled {checked}>{parse_inline(item_text)}</li>')
                else:
                    items.append(f'<li>{parse_inline(item_text)}</li>')
                i += 1
            html_lines.append('<ul>')
            html_lines.extend(items)
            html_lines.append('</ul>')
            continue

        # --- Ordered list ---
        ol_match = re.match(r'^(\s*)\d+\.\s+(.*)', line)
        if ol_match:
            items = []
            while i < n:
                m = re.match(r'^(\s*)\d+\.\s+(.*)', lines[i])
                if not m:
                    break
                items.append(f'<li>{parse_inline(m.group(2))}</li>')
                i += 1
            html_lines.append('<ol>')
            html_lines.extend(items)
            html_lines.append('</ol>')
            continue

        # --- Table ---
        if '|' in line and line.strip().startswith('|'):
            table_data = []
            header = line
            i += 1
            # Separator line
            if i < n and re.match(r'^\|[\s\-:|]+\|$', lines[i].strip()):
                i += 1
            else:
                # No separator, treat as regular text
                html_lines.append(parse_inline(line))
                continue

            # Parse header
            header_cells = [c.strip() for c in header.strip().strip('|').split('|')]

            # Parse data rows
            while i < n and '|' in lines[i] and lines[i].strip().startswith('|'):
                data_cells = [c.strip() for c in lines[i].strip().strip('|').split('|')]
                table_data.append(data_cells)
                i += 1

            html_lines.append('<table>')
            html_lines.append('<thead><tr>')
            for cell in header_cells:
                html_lines.append(f'<th>{parse_inline(cell)}</th>')
            html_lines.append('</tr></thead>')
            html_lines.append('<tbody>')
            for row in table_data:
                html_lines.append('<tr>')
                for cell in row:
                    html_lines.append(f'<td>{parse_inline(cell)}</td>')
                html_lines.append('</tr>')
            html_lines.append('</tbody></table>')
            continue

        # --- Paragraph / regular text ---
        para_lines = [line]
        i += 1
        # Collect consecutive non-empty non-special lines
        while i < n and lines[i].strip() != '' and \
              not lines[i].startswith('```') and \
              not re.match(r'^(#{1,6})\s', lines[i]) and \
              not lines[i].startswith('>') and \
              not re.match(r'^(\s*)[-*+]\s+', lines[i]) and \
              not re.match(r'^(\s*)\d+\.\s+', lines[i]) and \
              not (re.match(r'^[-*_]{3,}\s*$', lines[i].strip())) and \
              not (('|' in lines[i] and lines[i].strip().startswith('|') and i+1 < n and
                    re.match(r'^\|[\s\-:|]+\|$', lines[i+1].strip()))):
            para_lines.append(lines[i])
            i += 1
        para_text = '\n'.join(para_lines)
        para_html = convert_inline_md(para_text)
        html_lines.append(f'<p>{para_html}</p>')

    body = '\n'.join(html_lines)

    title = os.path.splitext(filename)[0].replace('_', ' ').title()

    return f'''<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>{escape(title)}</title>
{CSS_STYLE}
</head>
<body>
{body}
</body>
</html>'''


def convert_inline_md(text):
    """Convert inline markdown, handling line breaks within paragraphs."""
    lines = text.split('\n')
    result = []
    for line in lines:
        result.append(parse_inline(line))
    return '<br>\n'.join(result)


def main():
    md_files = sorted([f for f in os.listdir(BASE_DIR) if f.endswith('.md')])
    if not md_files:
        print("No .md files found.")
        return

    print(f"Found {len(md_files)} .md files to convert:\n")

    for md_file in md_files:
        md_path = os.path.join(BASE_DIR, md_file)
        html_file = md_file[:-3] + '.html'
        html_path = os.path.join(BASE_DIR, html_file)

        with open(md_path, 'r', encoding='utf-8') as f:
            md_text = f.read()

        html_text = convert_md_to_html(md_text, md_file)

        with open(html_path, 'w', encoding='utf-8') as f:
            f.write(html_text)

        print(f"  ✓ {md_file}  →  {html_file}")

    print(f"\nDone! Converted {len(md_files)} files.")


if __name__ == '__main__':
    main()
