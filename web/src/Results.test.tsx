import { renderToStaticMarkup } from "react-dom/server";
import { describe, expect, test } from "vitest";

import type { ResultFile, SearchResponse } from "./api";
import { FileResult, LINES_AT_ONCE, LineView, Results, filesToDraw, resultCount, type LinkFor } from "./Results";

// Links like the app makes, without its navigation.
const linkFor: LinkFor = (open) => ({
  href: `?file=${encodeURIComponent(open.path)}&line=${open.line ?? ""}`,
  onClick: () => undefined,
});

describe("escaping", () => {
  test("file content and paths never become HTML", () => {
    const file: ResultFile = {
      repo: "demo",
      path: '<img src=x onerror="alert(1)">.html',
      lines: [
        {
          line: 2,
          text: "and <script>alert(1)</script> &amp;",
          matches: [{ start: 4, end: 12 }],
        },
      ],
    };
    const html = renderToStaticMarkup(<FileResult file={file} />);
    expect(html).not.toContain("<script>");
    expect(html).not.toContain("<img");
    // The match is marked, with HTML's syntax colors inside it, all as text.
    expect(html).toMatch(/<mark>(<span[^>]*>[^<]*<\/span>)+<\/mark>/);
    expect(html.replace(/<[^>]*>/g, "")).toContain("and &lt;script&gt;alert(1)&lt;/script&gt; &amp;amp;");
    expect(html).toContain('class="hljs-tag"');
    expect(html).toContain("&lt;img src=x onerror=&quot;alert(1)&quot;&gt;.html");
    expect(html).toContain("&amp;amp;");
  });

  test("error, skipped and warning messages are text too", () => {
    const response: SearchResponse = {
      files: [],
      status: "error",
      error: "<b>bad</b>",
      skipped: [{ repo: "x", error: "x: <i>damaged</i>" }],
      warnings: ["<u>a/(b)/</u> is searched as plain text"],
    };
    const html = renderToStaticMarkup(<Results response={response} filesOnly={false} />);
    expect(html).toContain("&lt;b&gt;bad&lt;/b&gt;");
    expect(html).toContain("&lt;i&gt;damaged&lt;/i&gt;");
    expect(html).toContain("&lt;u&gt;a/(b)/&lt;/u&gt; is searched as plain text");
    expect(html).not.toContain("<b>");
  });
});

describe("results", () => {
  test("colors code by its file's language, with matches still marked", () => {
    const file: ResultFile = {
      repo: "r",
      path: "main.go",
      lines: [
        { line: 1, text: "/* a", matches: [] },
        { line: 2, text: "b */ func target() {}", matches: [{ start: 10, end: 16 }] },
      ],
    };
    const html = renderToStaticMarkup(<FileResult file={file} />);
    expect(html).toContain('<span class="hljs-comment">b */</span>');
    expect(html).toContain('<span class="hljs-function hljs-keyword">func</span>');
    expect(html).toContain('<mark><span class="hljs-function hljs-title">target</span></mark>');
  });

  test("marks a gap between lines that aren't next to each other", () => {
    const file: ResultFile = {
      repo: "",
      path: "a.txt",
      lines: [
        { line: 1, text: "one", matches: [{ start: 0, end: 3 }] },
        { line: 2, text: "two", matches: [] },
        { line: 9, text: "nine", matches: [{ start: 0, end: 4 }] },
      ],
    };
    const html = renderToStaticMarkup(<FileResult file={file} />);
    expect(html.match(/class="gap"/g)?.length).toBe(1);
    expect(html).not.toContain("repo-name"); // no repo for --path searches
  });

  test("links the file and each line", () => {
    const file: ResultFile = {
      repo: "r",
      path: "a.txt",
      lines: [{ line: 7, text: "x", matches: [{ start: 0, end: 1 }] }],
    };
    const html = renderToStaticMarkup(<FileResult file={file} linkFor={linkFor} />);
    expect(html).toContain('<a class="path" href="?file=a.txt&amp;line=">a.txt</a>');
    expect(html).toContain('<a class="line-number" href="?file=a.txt&amp;line=7">7</a>');
    // Without links, nothing is a link.
    expect(renderToStaticMarkup(<FileResult file={file} />)).not.toContain("<a ");
  });

  test("a path that isn't valid UTF-8 gets no link", () => {
    const file: ResultFile = { repo: "r", path: "a\uFFFD.txt", path_base64: "YYAudHh0", lines: [] };
    expect(renderToStaticMarkup(<FileResult file={file} linkFor={linkFor} />)).not.toContain("<a ");
  });

  test("a line in a file view can be picked and scrolled to", () => {
    const html = renderToStaticMarkup(
      <LineView line={{ line: 3, text: "y", matches: [] }} painted={[]} fadeContext={false} selected anchor />,
    );
    expect(html).toContain('class="line selected-line" id="L3"');
  });

  test("draws whole files up to the line cap, then offers more", () => {
    const file = (name: string, lines: number): ResultFile => ({
      repo: "r",
      path: name,
      lines: Array.from({ length: lines }, (_, i) => ({ line: i + 1, text: "x", matches: [] })),
    });
    const files = [file("a", 3), file("b", 3), file("c", 3)];
    expect(filesToDraw(files, 4).map((f) => f.path)).toEqual(["a", "b"]);
    expect(filesToDraw(files, 3).map((f) => f.path)).toEqual(["a"]);
    expect(filesToDraw([file("big", 50)], 1)).toHaveLength(1); // always one
    expect(filesToDraw([], 10)).toEqual([]);

    const many = Array.from({ length: 30 }, (_, i) => file(`f${i}`, 100));
    const html = renderToStaticMarkup(<Results response={{ files: many, status: "complete" }} filesOnly={false} />);
    const drawn = LINES_AT_ONCE / 100;
    expect(html.match(/class="file"/g)?.length).toBe(drawn);
    expect(html).toContain(`Show more (${30 - drawn} more files, ${(30 - drawn) * 100} lines)`);
  });

  test("plain text gets no element of its own", () => {
    const file: ResultFile = { repo: "r", path: "notes.txt", lines: [{ line: 1, text: "a b c", matches: [{ start: 2, end: 3 }] }] };
    expect(renderToStaticMarkup(<FileResult file={file} />)).toContain('<code class="line-text">a <mark>b</mark> c</code>');
  });

  test("counts results the way the limit does", () => {
    const files: ResultFile[] = [
      { repo: "r", path: "a", lines: [] }, // nothing to highlight: one result
      {
        repo: "r",
        path: "b",
        lines: [
          { line: 1, text: "x", matches: [{ start: 0, end: 1 }] },
          { line: 2, text: "y", matches: [] },
          { line: 3, text: "x", matches: [{ start: 0, end: 1 }] },
        ],
      },
    ];
    expect(resultCount(files)).toBe(3);
  });
});
