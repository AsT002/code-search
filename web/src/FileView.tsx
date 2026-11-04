// One whole file, as it was indexed, opened from a result. The line it was
// opened at is marked and scrolled to. Like the results, all file content
// is rendered as text.

import { useEffect, useMemo, useState } from "react";

import type { OpenFile } from "./address";
import { fetchFile, type ResultFile } from "./api";
import { LineView, paintLines, type LinkFor, type LinkProps } from "./Results";

function FileLines({ file, line, linkFor }: { file: ResultFile; line: number | null; linkFor: LinkFor }) {
  const painted = useMemo(() => paintLines(file), [file]);

  // Scroll to the line once it's there, and whenever another one is picked.
  useEffect(() => {
    if (line === null) {
      window.scrollTo(0, 0);
    } else {
      document.getElementById(`L${line}`)?.scrollIntoView({ block: "center" });
    }
  }, [file, line]);

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
          link={linkFor({ repo: file.repo, path: file.path, line: fileLine.line })}
          fadeContext={false}
          selected={fileLine.line === line}
          anchor
        />
      ))}
    </div>
  );
}

export function FileView({ open, linkFor, back }: { open: OpenFile; linkFor: LinkFor; back: LinkProps }) {
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
      {file !== null && <FileLines file={file} line={open.line} linkFor={linkFor} />}
    </section>
  );
}
