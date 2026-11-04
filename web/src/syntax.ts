// Syntax highlighting for result lines, with highlight.js through lowlight.
//
// lowlight gives back a tree of elements and text instead of an HTML
// string, so we turn it into plain tokens and render them as React text.
// File content never becomes HTML (see Results.tsx).

import type { ElementContent, RootContent } from "hast";
import { common, createLowlight } from "lowlight";

import type { Segment } from "./highlight";

// The ~37 common languages: C, C++, Go, Rust, Python, JS, TS, Java, ...
const lowlight = createLowlight(common);

// Longer blocks are shown plain: highlighting is regex based and runs on
// the page's only thread, so a huge minified line could freeze it.
const MAX_CHARS = 50_000;

// A piece of a line, and its highlight.js classes, like ["hljs-keyword"].
export interface Token {
  text: string;
  classes: string[];
}

// A match segment of a line, with the syntax tokens inside it.
export interface PaintedSegment {
  match: boolean;
  tokens: Token[];
}

// Files whose name, not extension, says the language.
const FILE_NAMES = new Map<string, string>([
  ["makefile", "makefile"],
  ["gnumakefile", "makefile"],
]);

// The language for a file, from its name or extension (highlight.js knows
// most extensions as names: go, rs, py, cpp, hpp, ...), or undefined.
export function languageFor(path: string): string | undefined {
  const name = path.slice(path.lastIndexOf("/") + 1).toLowerCase();
  const byName = FILE_NAMES.get(name);
  if (byName !== undefined) {
    return byName;
  }
  const dot = name.lastIndexOf(".");
  if (dot < 0) {
    return undefined;
  }
  const extension = name.slice(dot + 1);
  return extension !== "" && lowlight.registered(extension) ? extension : undefined;
}

function classesOf(node: ElementContent): string[] {
  if (node.type !== "element") {
    return [];
  }
  const names = node.properties.className;
  return Array.isArray(names) ? names.filter((name) => typeof name === "string") : [];
}

// Highlights lines that follow each other in the file as one block, so a
// comment or string that spans lines is colored right, and gives back each
// line's tokens. Lines without a language, or too long, are one plain token.
export function highlightLines(language: string | undefined, lines: string[]): Token[][] {
  const plain = () => lines.map((text) => [{ text, classes: [] }]);
  const code = lines.join("\n");
  if (language === undefined || code.length > MAX_CHARS) {
    return plain();
  }

  let current: Token[] = [];
  const out: Token[][] = [current];
  const walk = (nodes: RootContent[] | ElementContent[], classes: string[]) => {
    for (const node of nodes) {
      if (node.type === "text") {
        // A token can span lines: split it at each line end.
        node.value.split("\n").forEach((part, i) => {
          if (i > 0) {
            current = [];
            out.push(current);
          }
          if (part !== "") {
            current.push({ text: part, classes });
          }
        });
      } else if (node.type === "element") {
        walk(node.children, [...classes, ...classesOf(node)]);
      }
    }
  };
  try {
    walk(lowlight.highlight(language, code).children, []);
  } catch {
    return plain(); // highlight.js gave up: show the text as it is
  }
  return out.length === lines.length ? out : plain();
}

// Cuts a line's syntax tokens at the edges of its match segments, so each
// segment holds the tokens inside it. Both cover the same text.
export function paint(segments: Segment[], tokens: Token[]): PaintedSegment[] {
  const out: PaintedSegment[] = [];
  let t = 0; // the token we're in
  let used = 0; // how much of tokens[t].text earlier segments took
  for (const segment of segments) {
    const pieces: Token[] = [];
    let left = segment.text.length;
    for (let token = tokens[t]; left > 0 && token !== undefined; token = tokens[t]) {
      const take = Math.min(left, token.text.length - used);
      pieces.push({ text: token.text.slice(used, used + take), classes: token.classes });
      used += take;
      left -= take;
      if (used === token.text.length) {
        t++;
        used = 0;
      }
    }
    if (left > 0) {
      // Only if the tokens are shorter than the line, which shouldn't happen.
      pieces.push({ text: segment.text.slice(segment.text.length - left), classes: [] });
    }
    out.push({ match: segment.match, tokens: pieces });
  }
  return out;
}
