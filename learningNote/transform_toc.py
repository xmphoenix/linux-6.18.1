#!/usr/bin/env python3
"""
Transform HTML TOC from inline <details><summary> to fixed left sidebar.
References the structure from linux_scheduler_subsystem_guide.html.
"""

import re
import os
import sys

# CSS to add before </style> (sidebar styles)
SIDEBAR_CSS = """
/* ========== Sidebar — PDF-style left navigation ========== */
#sidebar {
    position: fixed;
    left: 0;
    top: 0;
    width: 300px;
    height: 100vh;
    background: #F5F0E8;
    border-right: 1px solid var(--border);
    overflow-y: auto;
    overflow-x: hidden;
    z-index: 1000;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Noto Sans", sans-serif;
    font-size: 0.82em;
    line-height: 1.5;
    box-shadow: 2px 0 8px rgba(0,0,0,0.04);
    transition: transform 0.3s ease;
}
#sidebar.sidebar-hidden {
    transform: translateX(-300px);
}

.sidebar-header {
    position: sticky;
    top: 0;
    background: #EDE5D8;
    padding: 14px 16px;
    font-weight: 700;
    font-size: 1.05em;
    color: var(--heading);
    border-bottom: 2px solid var(--accent);
    display: flex;
    justify-content: space-between;
    align-items: center;
    z-index: 10;
}

#sidebar-toggle-btn {
    background: none;
    border: 1px solid var(--border);
    border-radius: 4px;
    cursor: pointer;
    font-size: 1.1em;
    padding: 2px 8px;
    color: var(--text-muted);
    transition: all 0.15s;
}
#sidebar-toggle-btn:hover {
    background: var(--accent);
    color: #fff;
    border-color: var(--accent);
}

/* Floating toggle button when sidebar is hidden */
#sidebar-float-btn {
    display: none;
    position: fixed;
    left: 10px;
    top: 10px;
    z-index: 1001;
    background: var(--accent);
    color: #fff;
    border: none;
    border-radius: 50%;
    width: 40px;
    height: 40px;
    font-size: 1.2em;
    cursor: pointer;
    box-shadow: 0 2px 8px rgba(0,0,0,0.2);
    transition: all 0.2s;
    align-items: center;
    justify-content: center;
}
#sidebar-float-btn:hover {
    background: var(--accent-hover);
    transform: scale(1.05);
}

.toc-tree {
    padding: 8px 0 40px 0;
}
.toc-tree ul {
    list-style: none;
    padding: 0;
    margin: 0;
}
.toc-tree li {
    margin: 0;
}
.toc-tree ul li::marker { display: none; }

.toc-chapter {
    border-bottom: 1px solid var(--border-light);
}

.toc-chapter-header {
    display: flex;
    align-items: center;
    padding: 8px 14px;
    cursor: pointer;
    font-weight: 600;
    color: #3D3429;
    transition: background 0.12s;
    user-select: none;
}
.toc-chapter-header:hover {
    background: rgba(217, 117, 10, 0.06);
}
.toc-chapter-header .arrow {
    font-size: 0.65em;
    margin-right: 6px;
    transition: transform 0.2s;
    color: var(--text-muted);
    flex-shrink: 0;
}
.toc-chapter.expanded > .toc-chapter-header .arrow {
    transform: rotate(90deg);
}

.toc-chapter-items {
    display: none;
    background: rgba(0,0,0,0.015);
    padding: 2px 0 4px 0;
}
.toc-chapter.expanded > .toc-chapter-items {
    display: block;
}
.toc-chapter-items a {
    display: block;
    padding: 5px 14px 5px 32px;
    color: var(--text-muted);
    text-decoration: none;
    border-bottom: none;
    font-size: 0.95em;
    transition: all 0.12s;
    border-left: 2px solid transparent;
}
.toc-chapter-items a:hover {
    color: var(--accent);
    background: rgba(217, 117, 10, 0.04);
    border-left-color: var(--accent);
}
.toc-chapter-items a.active {
    color: var(--accent);
    background: var(--accent-soft);
    border-left-color: var(--accent);
    font-weight: 600;
}

/* ========== Responsive ========== */
@media (max-width: 900px) {
    body {
        margin-left: 0 !important;
    }
    #sidebar {
        transform: translateX(-300px);
    }
    #sidebar.sidebar-visible {
        transform: translateX(0);
    }
    #sidebar-float-btn {
        display: flex;
    }
}
"""

# JavaScript to add before </body>
SIDEBAR_JS = """
<script>
(function() {
    'use strict';

    var sidebar = document.getElementById('sidebar');
    var toggleBtn = document.getElementById('sidebar-toggle-btn');
    var floatBtn = document.getElementById('sidebar-float-btn');
    var chapters = document.querySelectorAll('.toc-chapter');
    var tocLinks = document.querySelectorAll('.toc-chapter-items a');

    // ---- Toggle sidebar visibility ----
    function hideSidebar() {
        sidebar.classList.add('sidebar-hidden');
        floatBtn.style.display = 'flex';
        document.body.style.marginLeft = '0';
    }
    function showSidebar() {
        sidebar.classList.remove('sidebar-hidden');
        sidebar.classList.add('sidebar-visible');
        floatBtn.style.display = 'none';
        if (window.innerWidth > 900) {
            document.body.style.marginLeft = '340px';
        }
    }

    toggleBtn.addEventListener('click', function(e) {
        e.stopPropagation();
        hideSidebar();
    });

    floatBtn.addEventListener('click', function() {
        showSidebar();
    });

    // ---- Chapter expand/collapse ----
    chapters.forEach(function(ch) {
        var header = ch.querySelector('.toc-chapter-header');
        header.addEventListener('click', function() {
            ch.classList.toggle('expanded');
        });
    });

    // ---- Scroll spy: highlight current section ----
    var headingIds = [];
    tocLinks.forEach(function(a) {
        var href = a.getAttribute('href');
        if (href && href.startsWith('#')) {
            headingIds.push(href.slice(1));
        }
    });

    function updateActiveLink() {
        var scrollY = window.scrollY + 120;
        var activeId = null;

        for (var i = headingIds.length - 1; i >= 0; i--) {
            var el = document.getElementById(headingIds[i]);
            if (el && el.offsetTop <= scrollY) {
                activeId = headingIds[i];
                break;
            }
        }

        tocLinks.forEach(function(a) {
            a.classList.remove('active');
            if (activeId && a.getAttribute('href') === '#' + activeId) {
                a.classList.add('active');
                var chapter = a.closest('.toc-chapter');
                if (chapter && !chapter.classList.contains('expanded')) {
                    chapter.classList.add('expanded');
                }
                var sidebarRect = sidebar.getBoundingClientRect();
                var linkRect = a.getBoundingClientRect();
                if (linkRect.bottom > sidebarRect.bottom - 20) {
                    a.scrollIntoView({ behavior: 'smooth', block: 'center' });
                } else if (linkRect.top < sidebarRect.top + 60) {
                    a.scrollIntoView({ behavior: 'smooth', block: 'center' });
                }
            }
        });
    }

    var scrollTicking = false;
    window.addEventListener('scroll', function() {
        if (!scrollTicking) {
            requestAnimationFrame(function() {
                updateActiveLink();
                scrollTicking = false;
            });
            scrollTicking = true;
        }
    });

    updateActiveLink();

    // ---- Keyboard shortcut: Ctrl+B to toggle sidebar ----
    document.addEventListener('keydown', function(e) {
        if (e.ctrlKey && e.key === 'b') {
            e.preventDefault();
            if (sidebar.classList.contains('sidebar-hidden')) {
                showSidebar();
            } else {
                hideSidebar();
            }
        }
    });

    // ---- Responsive: handle resize ----
    window.addEventListener('resize', function() {
        if (window.innerWidth > 900) {
            floatBtn.style.display = 'none';
            if (!sidebar.classList.contains('sidebar-hidden')) {
                document.body.style.marginLeft = '340px';
            }
        } else {
            document.body.style.marginLeft = '0';
            if (!sidebar.classList.contains('sidebar-hidden')) {
                floatBtn.style.display = 'flex';
            }
        }
    });
})();
</script>
"""


def extract_toc_section(content):
    """Find the TOC section from <h2>目录</h2> to the next <hr>"""
    toc_start_match = re.search(r'<h2>目录</h2>', content)
    if not toc_start_match:
        return None, None, None

    toc_start = toc_start_match.start()
    after_toc = content[toc_start_match.end():]

    # Find <hr> after toc_start (marks end of TOC)
    hr_match = re.search(r'<hr>', after_toc)
    if not hr_match:
        return None, None, None

    toc_end = toc_start_match.end() + hr_match.start()
    toc_content = content[toc_start:toc_end]

    return toc_start, toc_end, toc_content


def parse_toc_ol(toc_content):
    """Parse <ol>-based TOC: a flat <ol> with <li><a href="...">text</a></li> items."""
    ol_match = re.search(r'<ol>(.*?)</ol>', toc_content, re.DOTALL)
    if not ol_match:
        return None

    ol_content = ol_match.group(1)
    chapters = []
    for a in re.finditer(r'<a\s+href="([^"]*)"[^>]*>(.*?)</a>', ol_content, re.DOTALL):
        href = a.group(1)
        text = a.group(2).strip()
        # Strip inner tags like <span>, <strong>, etc.
        text = re.sub(r'<[/]?[^>]+>', '', text)
        chapters.append({
            'title': text,
            'href': href,
            'items': []
        })
    return chapters


def parse_toc_details(toc_content):
    """Parse the <details><summary> blocks from TOC content and extract chapters with items."""
    chapters = []

    # Pattern for each details block: <p><details><br>\n<summary>...</summary></p>\n...<ul>...</ul>\n<p></details></p>
    # Some have only summary without ul (standalone chapters)

    # Split by <p><details>
    blocks = re.split(r'<p><details>', toc_content)

    for block in blocks[1:]:  # skip content before first <p><details>
        # Find the summary
        summary_match = re.search(r'<summary>(.*?)</summary>', block, re.DOTALL)
        if not summary_match:
            continue

        summary_html = summary_match.group(1).strip()

        # Extract chapter title from summary
        # Pattern A: <a href="...">Title</a>
        # Pattern B: <b>Title</b>
        # Pattern C: <a href="..."><b>Title</b></a>
        title = None
        href = None

        # Try <a href="...">...</a>
        a_match = re.search(r'<a\s+href="([^"]*)"[^>]*>(.*?)</a>', summary_html, re.DOTALL)
        if a_match:
            href = a_match.group(1)
            inner = a_match.group(2).strip()
            # Strip any inner <b> tags
            inner = re.sub(r'<[/]?b>', '', inner)
            # Strip any <code> tags
            inner = re.sub(r'<[/]?code>', '', inner)
            title = inner
        else:
            # Try <b>Title</b>
            b_match = re.search(r'<b>(.*?)</b>', summary_html, re.DOTALL)
            if b_match:
                title = b_match.group(1).strip()
                # Strip any <code> tags
                title = re.sub(r'<[/]?code>', '', title)
            else:
                # Plain text
                title = re.sub(r'<br>', '', summary_html).strip()
                title = re.sub(r'<[/]?[^>]+>', '', title)

        if not title:
            continue

        # Extract list items (<a href="...">text</a>)
        items = []
        ul_match = re.search(r'<ul>(.*?)</ul>', block, re.DOTALL)
        if ul_match:
            ul_content = ul_match.group(1)
            # Find all <a href="...">text</a>
            for a in re.finditer(r'<a\s+href="([^"]*)"[^>]*>(.*?)</a>', ul_content, re.DOTALL):
                item_href = a.group(1)
                item_text = a.group(2).strip()
                # Strip inner tags
                item_text = re.sub(r'<[/]?[^>]+>', '', item_text)
                items.append((item_href, item_text))

        chapters.append({
            'title': title,
            'href': href,  # may be None
            'items': items
        })

    return chapters


def build_sidebar_html(chapters):
    """Build the sidebar <aside> HTML from parsed chapters."""
    lines = []
    lines.append('<aside id="sidebar">')
    lines.append('  <div class="sidebar-header">')
    lines.append('    <span>📑 目录</span>')
    lines.append('    <button id="sidebar-toggle-btn" title="隐藏目录">✕</button>')
    lines.append('  </div>')
    lines.append('  <nav class="toc-tree">')
    lines.append('    <ul>')

    first = True
    for i, ch in enumerate(chapters):
        expanded = ' expanded' if first else ''
        title_text = ch['title']
        # Remove leading numbering if the title is like "1. Title" or similar
        # Keep the original title as-is

        lines.append(f'      <li class="toc-chapter{expanded}">')
        lines.append(f'        <div class="toc-chapter-header"><span class="arrow">▶</span> {title_text}</div>')
        if ch['items']:
            lines.append('        <ul class="toc-chapter-items">')
            for href, text in ch['items']:
                lines.append(f'          <li><a href="{href}">{text}</a></li>')
            lines.append('        </ul>')
        lines.append('      </li>')
        first = False

    lines.append('    </ul>')
    lines.append('  </nav>')
    lines.append('</aside>')
    lines.append('<button id="sidebar-float-btn" title="显示目录">☰</button>')

    return '\n'.join(lines)


def transform_file(filepath):
    """Transform a single HTML file."""
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()

    # Check if already has sidebar
    if '<aside id="sidebar">' in content:
        print(f"  SKIP (already has sidebar): {filepath}")
        return

    # Check if has TOC
    if '<h2>目录</h2>' not in content:
        print(f"  SKIP (no TOC): {filepath}")
        return

    # === Step 1: Change body margin ===
    # Replace margin: 0 auto; with margin: 0 auto 0 340px;
    if 'margin: 0 auto 0 340px' not in content:
        content = content.replace('margin: 0 auto;', 'margin: 0 auto 0 340px;')

    # === Step 2: Add sidebar CSS before </style> ===
    if '#sidebar' not in content:
        # Find the first </style> (should be the main one)
        style_end = content.find('</style>')
        if style_end != -1:
            content = content[:style_end] + SIDEBAR_CSS + '\n' + content[style_end:]

    # === Step 3: Replace TOC with sidebar ===
    toc_start, toc_end, toc_content = extract_toc_section(content)
    if toc_start is None:
        print(f"  WARN: Could not extract TOC from {filepath}")
        return

    chapters = parse_toc_details(toc_content)
    if not chapters:
        # Try <ol>-based TOC
        chapters = parse_toc_ol(toc_content)
    if not chapters:
        print(f"  WARN: Could not parse chapters from {filepath}")
        return

    sidebar_html = build_sidebar_html(chapters)

    # Replace the entire TOC section (including <hr> after it)
    content = content[:toc_start] + sidebar_html + '\n\n' + content[toc_end:]

    # === Step 4: Add JavaScript before </body> ===
    if 'document.getElementById(\'sidebar\')' not in content:
        body_end = content.rfind('</body>')
        if body_end != -1:
            content = content[:body_end] + SIDEBAR_JS + '\n' + content[body_end:]

    # Write back
    with open(filepath, 'w', encoding='utf-8') as f:
        f.write(content)

    print(f"  OK: {filepath} ({len(chapters)} chapters)")


def main():
    learning_note_dir = '/home/ybzhang/kernel/linux-6.18.1/learningNote'

    # Get all HTML files with TOC
    html_files = []
    for f in sorted(os.listdir(learning_note_dir)):
        if f.endswith('.html'):
            html_files.append(os.path.join(learning_note_dir, f))

    print(f"Found {len(html_files)} HTML files")

    for filepath in html_files:
        transform_file(filepath)

    print("\nDone!")


if __name__ == '__main__':
    main()
