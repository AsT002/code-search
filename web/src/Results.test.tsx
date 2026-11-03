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
    expect(html).toContain("<mark>&lt;script&gt;</mark>");
    expect(html).toContain("&lt;img src=x onerror=&quot;alert(1)&quot;&gt;.html");
    expect(html).toContain("&amp;amp;");
  });

  test("error and skipped messages are text too", () => {
    const response: SearchResponse = {
      files: [],
      status: "error",
      error: "<b>bad</b>",
      skipped: [{ repo: "x", error: "x: <i>damaged</i>" }],
    };
    const html = renderToStaticMarkup(<Results response={response} filesOnly={false} />);
    expect(html).toContain("&lt;b&gt;bad&lt;/b&gt;");
    expect(html).toContain("&lt;i&gt;damaged&lt;/i&gt;");
    expect(html).not.toContain("<b>");
  });
});

describe("results", () => {
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
