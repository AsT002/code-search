// The result list. Every piece of text from a file or path is rendered as a
// React text node, which React escapes, so file content can never become
// HTML. Nothing here may use dangerouslySetInnerHTML.

import { useMemo, type MouseEvent, type ReactNode } from "react";

import type { OpenFile } from "./address";
import type { ResultFile, ResultLine, SearchResponse } from "./api";
import { lineBytes, segments, type Segment } from "./highlight";
import { highlightLines, languageFor, paint, type PaintedSegment } from "./syntax";

// A link inside the page: a real href, so it can open in a new tab, and an
// onClick that shows it without loading the page again.
export interface LinkProps {
  href: string;
  onClick: (event: MouseEvent) => void;
}

// Makes the link for opening a file, or a line in it.
export type LinkFor = (open: OpenFile) => LinkProps;

// Follows `link` on a click on the line's text, unless the click ended a
// text selection: people select code to copy it.
function followUnlessSelecting(link: LinkProps) {
  return (event: MouseEvent) => {
    if ((window.getSelection()?.toString() ?? "") === "") {
      link.onClick(event);
    }
  };
}

export function LineView({
  line,
  painted,
  link,
  fadeContext = true,
  selected = false,
  anchor = false,
}: {
  line: ResultLine;
  painted: PaintedSegment[];
  link?: LinkProps; // where clicking the line goes
  fadeContext?: boolean; // show lines without matches faded, as context
  selected?: boolean; // the line a file was opened at
  anchor?: boolean; // give the line an id, L<number>, to scroll to
}) {
  let className = "line";
  if (line.matches.length > 0) {
    className += " match-line";
  } else if (fadeContext) {
    className += " context-line";
  }
  if (selected) {
    className += " selected-line";
  }
  if (link !== undefined) {
    className += " linked-line";
  }
  return (
    <div className={className} id={anchor ? `L${line.line}` : undefined}>
      {link === undefined ? (
        <span className="line-number">{line.line}</span>
      ) : (
        <a className="line-number" href={link.href} onClick={link.onClick}>
          {line.line}
        </a>
      )}
      <code className="line-text" onClick={link === undefined ? undefined : followUnlessSelecting(link)}>
        {painted.map((segment, i) => {
          const tokens = segment.tokens.map((token, k) => (
            <span key={k} className={token.classes.length > 0 ? token.classes.join(" ") : undefined}>
              {token.text}
            </span>
          ));
          return segment.match ? <mark key={i}>{tokens}</mark> : <span key={i}>{tokens}</span>;
        })}
      </code>
    </div>
  );
}

// Each line's match segments, with syntax colors inside them. Lines that
// follow each other in the file are colored as one block.
export function paintLines(file: ResultFile): PaintedSegment[][] {
  const language = languageFor(file.path);
  const out: PaintedSegment[][] = [];
  let block: Segment[][] = []; // the match segments of each line in this block
  let previous = 0;
  const finishBlock = () => {
    const tokens = highlightLines(
      language,
      block.map((pieces) => pieces.map((piece) => piece.text).join("")),
    );
    block.forEach((pieces, i) => out.push(paint(pieces, tokens[i] ?? [])));
    block = [];
  };
  for (const line of file.lines) {
    if (block.length > 0 && line.line !== previous + 1) {
      finishBlock();
    }
    block.push(segments(lineBytes(line), line.matches));
    previous = line.line;
  }
  finishBlock();
  return out;
}

// A path that wasn't valid UTF-8 shows with U+FFFD in it, so it can't be
// asked for again: such files get no link.
function canLink(file: ResultFile): boolean {
  return file.repo !== "" && file.path_base64 === undefined;
}

export function FileResult({ file, linkFor }: { file: ResultFile; linkFor?: LinkFor }) {
  const painted = useMemo(() => paintLines(file), [file]);
  const links = linkFor !== undefined && canLink(file) ? linkFor : undefined;
  const rows: ReactNode[] = [];
  let previous = 0;
  file.lines.forEach((line, i) => {
    // A gap between groups of lines that aren't next to each other.
    if (previous !== 0 && line.line > previous + 1) {
      rows.push(<div key={`gap-${line.line}`} className="gap" aria-hidden="true" />);
    }
    const link = links?.({ repo: file.repo, path: file.path, line: line.line });
    rows.push(<LineView key={line.line} line={line} painted={painted[i] ?? []} link={link} />);
    previous = line.line;
  });
  const fileLink = links?.({ repo: file.repo, path: file.path, line: null });
  return (
    <section className="file">
      <h2 className="file-name">
        {file.repo !== "" && <span className="repo-name">{file.repo}</span>}
        {fileLink === undefined ? (
          <span className="path">{file.path}</span>
        ) : (
          <a className="path" href={fileLink.href} onClick={fileLink.onClick}>
            {file.path}
          </a>
        )}
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
      {(response.warnings ?? []).map((warning) => (
        <p key={warning} className="warning">
          {warning}
        </p>
      ))}
    </div>
  );
}

export function Results({
  response,
  filesOnly,
  linkFor,
}: {
  response: SearchResponse;
  filesOnly: boolean;
  linkFor?: LinkFor;
}) {
  return (
    <div className="results">
      <Summary response={response} filesOnly={filesOnly} />
      {response.files.map((file) => (
        <FileResult key={`${file.repo}\u0000${file.path_base64 ?? file.path}`} file={file} linkFor={linkFor} />
      ))}
    </div>
  );
}
