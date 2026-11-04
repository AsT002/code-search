// The page's address holds the search and the open file, so a reload, a
// shared link or the back button shows the same thing:
//
//   ?q=foo&repo=redis                          a search
//   ?q=foo&file_repo=redis&file=src/a.c&line=12  a file opened from it

import type { SearchParams } from "./api";

export const DEFAULTS: SearchParams = {
  query: "",
  repo: "",
  caseSensitive: false,
  filesOnly: false,
  context: 0,
  maxResults: 200,
};

// A file to show, and the line to go to (null for its top).
export interface OpenFile {
  repo: string;
  path: string;
  line: number | null;
}

export interface Address {
  params: SearchParams;
  open: OpenFile | null;
}

// The whole number in `text`, like "12", or null if it isn't one: "",
// "-1", "1.5" and "1e3" aren't.
export function countIn(text: string): number | null {
  const trimmed = text.trim();
  if (!/^\d+$/.test(trimmed)) {
    return null;
  }
  const count = Number(trimmed);
  return Number.isSafeInteger(count) ? count : null;
}

export function readAddress(search: string): Address {
  const address = new URLSearchParams(search);
  const count = (name: string, fallback: number) => countIn(address.get(name) ?? "") ?? fallback;
  const params: SearchParams = {
    query: address.get("q") ?? "",
    repo: address.get("repo") ?? "",
    caseSensitive: address.get("case") === "yes",
    filesOnly: address.get("files") === "yes",
    context: count("context", DEFAULTS.context),
    maxResults: count("max", DEFAULTS.maxResults),
  };
  const repo = address.get("file_repo");
  const path = address.get("file");
  const line = count("line", 0);
  const open = repo && path !== null ? { repo, path, line: line > 0 ? line : null } : null;
  return { params, open };
}

// The address for a search and an open file, like "?q=foo&file=a.c".
export function addressFor(params: SearchParams, open: OpenFile | null): string {
  const address = new URLSearchParams();
  if (params.query !== "") address.set("q", params.query);
  if (params.repo !== "") address.set("repo", params.repo);
  if (params.caseSensitive) address.set("case", "yes");
  if (params.filesOnly) address.set("files", "yes");
  if (params.context !== DEFAULTS.context) address.set("context", String(params.context));
  if (params.maxResults !== DEFAULTS.maxResults) address.set("max", String(params.maxResults));
  if (open !== null) {
    address.set("file_repo", open.repo);
    address.set("file", open.path);
    if (open.line !== null) address.set("line", String(open.line));
  }
  const text = address.toString();
  return text === "" ? "./" : `?${text}`;
}

export function sameParams(a: SearchParams, b: SearchParams): boolean {
  return (
    a.query === b.query &&
    a.repo === b.repo &&
    a.caseSensitive === b.caseSensitive &&
    a.filesOnly === b.filesOnly &&
    a.context === b.context &&
    a.maxResults === b.maxResults
  );
}
