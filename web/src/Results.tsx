// The result list. Every piece of text from a file or path is rendered as a
// React text node, which React escapes, so file content can never become
// HTML. Nothing here may use dangerouslySetInnerHTML.

import { Fragment, memo, useMemo, useState, type MouseEvent, type ReactNode } from "react";

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

// One line. memo: it renders again only when its props change, so typing
// in the search box or picking another line doesn't redraw every line.
export const LineView = memo(function LineView({
  line,
  painted,
  repo = "",
  path = "",
  linkFor,
  fadeContext = true,
  selected = false,
  anchor = false,
}: {
  line: ResultLine;
  painted: PaintedSegment[];
  repo?: string; // with path and linkFor: clicking the line opens it there
  path?: string;
  linkFor?: LinkFor;
  fadeContext?: boolean; // show lines without matches faded, as context
  selected?: boolean; // the line a file was opened at
  anchor?: boolean; // give the line an id, L<number>, to scroll to
}) {
  const link = linkFor?.({ repo, path, line: line.line });
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
          // Only colored text gets an element: a long result list has
          // hundreds of thousands of pieces, and each element costs.
          const tokens = segment.tokens.map((token, k) =>
            token.classes.length > 0 ? (
              <span key={k} className={token.classes.join(" ")}>
                {token.text}
              </span>
            ) : (
              <Fragment key={k}>{token.text}</Fragment>
            ),
          );
          return segment.match ? <mark key={i}>{tokens}</mark> : <Fragment key={i}>{tokens}</Fragment>;
        })}
      </code>
    </div>
  );
});

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

export const FileResult = memo(function FileResult({ file, linkFor }: { file: ResultFile; linkFor?: LinkFor }) {
  const painted = useMemo(() => paintLines(file), [file]);
  const links = linkFor !== undefined && canLink(file) ? linkFor : undefined;
  const rows: ReactNode[] = [];
  let previous = 0;
  file.lines.forEach((line, i) => {
    // A gap between groups of lines that aren't next to each other.
    if (previous !== 0 && line.line > previous + 1) {
      rows.push(<div key={`gap-${line.line}`} className="gap" aria-hidden="true" />);
    }
    rows.push(
      <LineView
        key={line.line}
        line={line}
        painted={painted[i] ?? []}
        repo={file.repo}
        path={file.path}
        linkFor={links}
      />,
    );
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
});

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
  // Drawing every line of a big result would freeze the page, so files are
  // drawn until about LINES_AT_ONCE lines are, and a button adds more. The
  // count goes back to the start for each new response.
  const [shown, setShown] = useState({ response, lines: LINES_AT_ONCE });
  const maxLines = shown.response === response ? shown.lines : LINES_AT_ONCE;
  const files = filesToDraw(response.files, maxLines);
  const linesLeft = countLines(response.files.slice(files.length));
  return (
    <div className="results">
      <Summary response={response} filesOnly={filesOnly} />
      {files.map((file) => (
        <FileResult key={`${file.repo}\u0000${file.path_base64 ?? file.path}`} file={file} linkFor={linkFor} />
      ))}
      {files.length < response.files.length && (
        <button
          type="button"
          className="show-more"
          onClick={() => setShown({ response, lines: maxLines + LINES_AT_ONCE })}
        >
          Show more ({plural(response.files.length - files.length, "more file")},{" "}
          {plural(linesLeft, "line")})
        </button>
      )}
    </div>
  );
}

// How many result lines are drawn at first, and added by "Show more".
export const LINES_AT_ONCE = 2000;

function countLines(files: ResultFile[]): number {
  return files.reduce((count, file) => count + Math.max(file.lines.length, 1), 0);
}

// The first files, whole, until they have at least `maxLines` lines (a file
// with no lines counts as one). Always at least one file.
export function filesToDraw(files: ResultFile[], maxLines: number): ResultFile[] {
  let lines = 0;
  let count = 0;
  while (count < files.length && (count === 0 || lines < maxLines)) {
    lines += Math.max(files[count]?.lines.length ?? 0, 1);
    count++;
  }
  return files.slice(0, count);
}
