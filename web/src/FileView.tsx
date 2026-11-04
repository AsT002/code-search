// One whole file, as it was indexed, opened from a result. The line it was
// opened at is marked and scrolled to. Like the results, all file content
// is rendered as text.

import { useEffect, useMemo, useState } from "react";

import type { OpenFile } from "./address";
import { fetchFile, type ResultFile } from "./api";
import { LineView, paintLines, type LinkFor, type LinkProps } from "./Results";

function FileLines({
  file,
  line,
  jump,
  linkFor,
}: {
  file: ResultFile;
  line: number | null;
  jump: boolean;
  linkFor: LinkFor;
}) {
  const painted = useMemo(() => paintLines(file), [file]);

  // Scroll to the line once the file is there, and when the address picks
  // another one. Not when it was picked by a click (jump is false).
  useEffect(() => {
    if (!jump) {
      return;
    }
    if (line === null) {
      window.scrollTo(0, 0);
    } else {
      document.getElementById(`L${line}`)?.scrollIntoView({ block: "center" });
    }
  }, [file, line, jump]);

  if (file.lines.length === 0) {
    return <p className="muted">The file is empty.</p>;
  }
  return (
    <div className="lines">
      {file.lines.map((fileLine, i) => (
        <LineView
          key={fileLine.line}
          line={fileLine}
          painted={painted[i] ?? []}
          repo={file.repo}
          path={file.path}
          linkFor={linkFor}
          fadeContext={false}
          selected={fileLine.line === line}
          anchor
        />
      ))}
    </div>
  );
}

export function FileView({
  open,
  jump,
  linkFor,
  back,
}: {
  open: OpenFile;
  jump: boolean;
  linkFor: LinkFor;
  back: LinkProps;
}) {
  const [file, setFile] = useState<ResultFile | null>(null);
  const [error, setError] = useState("");

  useEffect(() => {
    const controller = new AbortController();
    setFile(null);
    setError("");
    fetchFile(open.repo, open.path, controller.signal).then(setFile, (problem: unknown) => {
      if (!controller.signal.aborted) {
        setError(problem instanceof Error ? problem.message : String(problem));
      }
    });
    return () => controller.abort();
  }, [open.repo, open.path]);

  return (
    <section className="file file-view">
      <p className="back">
        <a href={back.href} onClick={back.onClick}>
          ← Back to results
        </a>
      </p>
      <h2 className="file-name">
        <span className="repo-name">{open.repo}</span>
        <span className="path">{open.path}</span>
      </h2>
      {error !== "" && (
        <p className="error" role="alert">
          {error}
        </p>
      )}
      {error === "" && file === null && <p className="muted">Loading…</p>}
      {file !== null && <FileLines file={file} line={open.line} jump={jump} linkFor={linkFor} />}
    </section>
  );
}
