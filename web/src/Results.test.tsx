import { renderToStaticMarkup } from "react-dom/server";
import { describe, expect, test } from "vitest";

import type { ResultFile, SearchResponse } from "./api";
import { FileResult, Results, resultCount } from "./Results";

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
