// Turning a result line and its byte offsets into text to show.
//
// The server gives match positions as byte offsets into the line's UTF-8
// bytes, but JavaScript strings count UTF-16 code units. So we work on the
// bytes, cut them at the offsets, and decode each piece.

import type { Match, ResultLine } from "./api";

export interface Segment {
  text: string;
  match: boolean;
}

function base64ToBytes(base64: string): Uint8Array {
  const binary = atob(base64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) {
    bytes[i] = binary.charCodeAt(i);
  }
  return bytes;
}

// The line's exact bytes: from text_base64 when the text had bytes that
// aren't valid UTF-8, otherwise from the text itself.
export function lineBytes(line: ResultLine): Uint8Array {
  if (line.text_base64 !== undefined) {
    return base64ToBytes(line.text_base64);
  }
  return new TextEncoder().encode(line.text);
}

// A byte in the middle of a UTF-8 character: 10xxxxxx.
function isContinuation(byte: number | undefined): boolean {
  return byte !== undefined && (byte & 0xc0) === 0x80;
}

// Splits `bytes` into plain and highlighted pieces. A highlight that starts
// or ends inside a character covers the whole character, as in the CLI.
// Overlapping highlights are joined. Bytes that aren't valid UTF-8 show as
// U+FFFD.
export function segments(bytes: Uint8Array, matches: Match[]): Segment[] {
  const ranges: { start: number; end: number }[] = [];
  for (const match of matches) {
    let start = Math.max(0, Math.min(match.start, bytes.length));
    let end = Math.max(0, Math.min(match.end, bytes.length));
    while (start > 0 && isContinuation(bytes[start])) {
      start--;
    }
    while (end < bytes.length && isContinuation(bytes[end])) {
      end++;
    }
    if (end > start) {
      ranges.push({ start, end });
    }
  }
  ranges.sort((a, b) => a.start - b.start);

  const joined: { start: number; end: number }[] = [];
  for (const range of ranges) {
    const last = joined[joined.length - 1];
    if (last !== undefined && range.start <= last.end) {
      last.end = Math.max(last.end, range.end);
    } else {
      joined.push({ ...range });
    }
  }

  const decoder = new TextDecoder(); // replaces bad bytes with U+FFFD
  const decode = (start: number, end: number) => decoder.decode(bytes.subarray(start, end));
  const out: Segment[] = [];
  let position = 0;
  for (const range of joined) {
    if (range.start > position) {
      out.push({ text: decode(position, range.start), match: false });
    }
    out.push({ text: decode(range.start, range.end), match: true });
    position = range.end;
  }
  if (position < bytes.length || out.length === 0) {
    out.push({ text: decode(position, bytes.length), match: false });
  }
  return out;
}
