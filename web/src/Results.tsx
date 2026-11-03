// The result list. Every piece of text from a file or path is rendered as a
// React text node, which React escapes, so file content can never become
// HTML. Nothing here may use dangerouslySetInnerHTML.

import type { ReactNode } from "react";

import type { ResultFile, ResultLine, SearchResponse } from "./api";
import { lineBytes, segments } from "./highlight";

function LineView({ line }: { line: ResultLine }) {
  const isMatch = line.matches.length > 0;
  return (
    <div className={isMatch ? "line match-line" : "line context-line"}>
      <span className="line-number">{line.line}</span>
      <code className="line-text">
        {segments(lineBytes(line), line.matches).map((segment, i) =>
          segment.match ? <mark key={i}>{segment.text}</mark> : <span key={i}>{segment.text}</span>,
        )}
      </code>
    </div>
  );
}

export function FileResult({ file }: { file: ResultFile }) {
  const rows: ReactNode[] = [];
  let previous = 0;
  for (const line of file.lines) {
    // A gap between groups of lines that aren't next to each other.
    if (previous !== 0 && line.line > previous + 1) {
      rows.push(<div key={`gap-${line.line}`} className="gap" aria-hidden="true" />);
    }
    rows.push(<LineView key={line.line} line={line} />);
    previous = line.line;
  }
  return (
    <section className="file">
      <h2 className="file-name">
        {file.repo !== "" && <span className="repo-name">{file.repo}</span>}
        <span className="path">{file.path}</span>
      </h2>
      {rows.length > 0 && <div className="lines">{rows}</div>}
    </section>
  );
}

// How many results the server counted toward the limit: matching lines,
// or one for a file with nothing to highlight.
export function resultCount(files: ResultFile[]): number {
  let count = 0;
  for (const file of files) {
    const matching = file.lines.filter((line) => line.matches.length > 0).length;
    count += matching > 0 ? matching : 1;
  }
  return count;
}

function plural(count: number, word: string): string {
  return `${count} ${word}${count === 1 ? "" : "s"}`;
}

export function Summary({ response, filesOnly }: { response: SearchResponse; filesOnly: boolean }) {
  const files = response.files.length;
  let text = filesOnly
    ? plural(files, "file")
    : `${plural(resultCount(response.files), "result")} in ${plural(files, "file")}`;
  if (response.status === "truncated") {
    text += ". Stopped at the limit; there are more.";
  } else if (response.status === "cancelled") {
    text += ". The search was cancelled.";
  }
  return (
    <div className="summary">
      {response.status !== "error" && <p>{text}</p>}
      {response.status === "error" && (
        <p className="error" role="alert">
          {response.error ?? "the search failed"}
        </p>
      )}
      {(response.skipped ?? []).map((skipped) => (
        <p key={skipped.repo} className="warning">
          Skipped {skipped.error}
        </p>
      ))}
    </div>
  );
}

export function Results({ response, filesOnly }: { response: SearchResponse; filesOnly: boolean }) {
  return (
    <div className="results">
      <Summary response={response} filesOnly={filesOnly} />
      {response.files.map((file) => (
        <FileResult key={`${file.repo}\u0000${file.path_base64 ?? file.path}`} file={file} />
      ))}
    </div>
  );
}
