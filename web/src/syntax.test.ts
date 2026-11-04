import { describe, expect, test } from "vitest";

import { highlightLines, languageFor, paint, type Token } from "./syntax";

// The classes on the token that holds `text`, in the line's tokens.
function classesAt(tokens: Token[] | undefined, text: string): string[] | undefined {
  return tokens?.find((token) => token.text.includes(text))?.classes;
}

describe("languageFor", () => {
  test("knows languages by extension and by name", () => {
    expect(languageFor("src/main.go")).toBe("go");
    expect(languageFor("a/b/lib.RS")).toBe("rs");
    expect(languageFor("include/x.hpp")).toBe("hpp");
    expect(languageFor("Makefile")).toBe("makefile");
  });

  test("leaves unknown files plain", () => {
    expect(languageFor("LICENSE")).toBeUndefined();
    expect(languageFor("data.nope")).toBeUndefined();
    expect(languageFor("dir.go/README")).toBeUndefined();
    expect(languageFor("x.")).toBeUndefined();
    expect(languageFor("x.constructor")).toBeUndefined();
  });
});

describe("highlightLines", () => {
  test("gives each line its tokens, with the same text", () => {
    const lines = ["func main() {", '\treturn "hi" // done', "}"];
    const tokens = highlightLines("go", lines);
    expect(tokens.map((line) => line.map((token) => token.text).join(""))).toEqual(lines);
    expect(classesAt(tokens[0], "func")).toContain("hljs-keyword");
    expect(classesAt(tokens[1], '"hi"')).toContain("hljs-string");
    expect(classesAt(tokens[1], "// done")).toContain("hljs-comment");
  });

  test("colors a comment that spans lines on each of them", () => {
    const tokens = highlightLines("c", ["/* one", "two */ int x;"]);
    expect(classesAt(tokens[0], "one")).toContain("hljs-comment");
    expect(classesAt(tokens[1], "two")).toContain("hljs-comment");
    expect(classesAt(tokens[1], "int")).toContain("hljs-type");
  });

  test("leaves text plain without a language or when it's too long", () => {
    expect(highlightLines(undefined, ["int x;"])).toEqual([[{ text: "int x;", classes: [] }]]);
    const long = "x".repeat(60_000);
    expect(highlightLines("c", [long])).toEqual([[{ text: long, classes: [] }]]);
  });

  test("keeps empty lines", () => {
    const tokens = highlightLines("go", ["a", "", "b"]);
    expect(tokens).toHaveLength(3);
    expect(tokens[1]).toEqual([]);
  });
});

describe("paint", () => {
  test("cuts tokens at the match edges", () => {
    const tokens: Token[] = [
      { text: "return", classes: ["k"] },
      { text: " ", classes: [] },
      { text: "value", classes: ["v"] },
    ];
    const painted = paint(
      [
        { text: "ret", match: false },
        { text: "urn va", match: true },
        { text: "lue", match: false },
      ],
      tokens,
    );
    expect(painted).toEqual([
      { match: false, tokens: [{ text: "ret", classes: ["k"] }] },
      {
        match: true,
        tokens: [
          { text: "urn", classes: ["k"] },
          { text: " ", classes: [] },
          { text: "va", classes: ["v"] },
        ],
      },
      { match: false, tokens: [{ text: "lue", classes: ["v"] }] },
    ]);
  });

  test("an empty line is one empty segment", () => {
    expect(paint([{ text: "", match: false }], [])).toEqual([{ match: false, tokens: [] }]);
  });
});
