import { describe, expect, test } from "vitest";

import { lineBytes, segments } from "./highlight";

const bytes = (text: string) => new TextEncoder().encode(text);

describe("segments", () => {
  test("splits a line at byte offsets", () => {
    expect(segments(bytes("second Target line"), [{ start: 7, end: 13 }])).toEqual([
      { text: "second ", match: false },
      { text: "Target", match: true },
      { text: " line", match: false },
    ]);
  });

  test("counts bytes, not UTF-16 units", () => {
    // "é" is 2 bytes and "😀" 4 bytes, but 1 and 2 UTF-16 units.
    expect(segments(bytes("é😀 needle"), [{ start: 7, end: 13 }])).toEqual([
      { text: "é😀 ", match: false },
      { text: "needle", match: true },
    ]);
  });

  test("widens a highlight that cuts a character", () => {
    // Bytes 1 to 2 are the middle of "é" (C3 A9) and the start of "x".
    expect(segments(bytes("aéx"), [{ start: 2, end: 3 }])).toEqual([
      { text: "a", match: false },
      { text: "é", match: true },
      { text: "x", match: false },
    ]);
  });

  test("joins overlapping highlights and drops empty ones", () => {
    expect(
      segments(bytes("abcdef"), [
        { start: 1, end: 3 },
        { start: 2, end: 4 },
        { start: 5, end: 5 },
      ]),
    ).toEqual([
      { text: "a", match: false },
      { text: "bcd", match: true },
      { text: "ef", match: false },
    ]);
  });

  test("keeps offsets past the end inside the line", () => {
    expect(segments(bytes("ab"), [{ start: 1, end: 99 }])).toEqual([
      { text: "a", match: false },
      { text: "b", match: true },
    ]);
  });

  test("gives one empty piece for an empty line", () => {
    expect(segments(bytes(""), [])).toEqual([{ text: "", match: false }]);
  });
});

describe("lineBytes", () => {
  test("uses the exact bytes when the text wasn't valid UTF-8", () => {
    // "target \xFF\xFE bytes": the server sends U+FFFD in text, and the
    // real bytes in text_base64. Offsets are into the real bytes.
    const line = {
      line: 1,
      text: "target �� bytes",
      text_base64: "dGFyZ2V0IP/+IGJ5dGVz",
      matches: [{ start: 10, end: 15 }],
    };
    expect(lineBytes(line).length).toBe(15);
    expect(segments(lineBytes(line), line.matches)).toEqual([
      { text: "target �� ", match: false },
      { text: "bytes", match: true },
    ]);
  });
});

describe("byte order marks", () => {
  test("a U+FEFF inside a line is kept, matched or not", () => {
    const bytes = new Uint8Array([0x61, 0xef, 0xbb, 0xbf, 0x62]); // a, U+FEFF, b
    expect(segments(bytes, [{ start: 1, end: 5 }])).toEqual([
      { text: "a", match: false },
      { text: "﻿b", match: true },
    ]);
    expect(segments(new Uint8Array([0xef, 0xbb, 0xbf, 0x78]), [])).toEqual([{ text: "﻿x", match: false }]);
  });
});
