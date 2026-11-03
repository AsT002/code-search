// Calls to the code-search HTTP API (see "HTTP API" in docs/ARCHITECTURE.md).

export interface Match {
  start: number; // byte offsets in the line's original bytes
  end: number;
}

export interface ResultLine {
  line: number;
  text: string;
  text_base64?: string; // the exact bytes, when they aren't valid UTF-8
  matches: Match[]; // empty for a context line
}

export interface ResultFile {
  repo: string;
  path: string;
  path_base64?: string;
  lines: ResultLine[];
}

export type SearchStatus = "complete" | "truncated" | "cancelled" | "error";

// The same document `code-search search --json` prints.
export interface SearchResponse {
  files: ResultFile[];
  status: SearchStatus;
  error?: string;
  skipped?: { repo: string; error: string }[];
}

export interface SearchParams {
  query: string;
  repo: string; // "" means every repo
  caseSensitive: boolean;
  filesOnly: boolean;
  context: number;
  maxResults: number;
}

export interface Repo {
  name: string;
  source: "local" | "github";
  location: string;
  commit: string;
}

export function searchUrl(params: SearchParams): string {
  const query = new URLSearchParams({ q: params.query });
  if (params.repo !== "") {
    query.set("repo", params.repo);
  }
  if (params.caseSensitive) {
    query.set("case_sensitive", "true");
  }
  if (params.filesOnly) {
    query.set("files_only", "true");
  }
  if (params.context > 0) {
    query.set("context", String(params.context));
  }
  query.set("max_results", String(params.maxResults));
  return `/api/v1/search?${query.toString()}`;
}

// Errors outside a search come as {"error":{"code":...,"message":...}}.
function errorMessage(body: unknown, fallback: string): string {
  if (typeof body === "object" && body !== null && "error" in body) {
    const error = (body as { error: unknown }).error;
    if (typeof error === "string") {
      return error;
    }
    if (typeof error === "object" && error !== null && "message" in error) {
      return String((error as { message: unknown }).message);
    }
  }
  return fallback;
}

// Runs a search. Aborting `signal` closes the connection, which stops the
// search on the server too. Errors come back as a document with
// status "error", like the server's own.
export async function search(params: SearchParams, signal: AbortSignal): Promise<SearchResponse> {
  const response = await fetch(searchUrl(params), { signal });
  let body: unknown;
  try {
    body = await response.json();
  } catch (error) {
    if (signal.aborted) {
      throw error;
    }
    return { files: [], status: "error", error: `the server answered ${response.status}` };
  }
  if (typeof body === "object" && body !== null && "files" in body) {
    return body as SearchResponse;
  }
  return { files: [], status: "error", error: errorMessage(body, response.statusText) };
}

export async function listRepos(): Promise<Repo[]> {
  const response = await fetch("/api/v1/repos");
  const body: unknown = await response.json();
  if (!response.ok) {
    throw new Error(errorMessage(body, response.statusText));
  }
  return (body as { repos: Repo[] }).repos;
}
