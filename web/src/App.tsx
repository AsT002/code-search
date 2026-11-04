import { useCallback, useEffect, useLayoutEffect, useRef, useState, type MouseEvent } from "react";

import { addressFor, countIn, readAddress, sameParams, type OpenFile } from "./address";
import { listRepos, search, type Repo, type SearchParams, type SearchResponse } from "./api";
import { FileView } from "./FileView";
import { Results, type LinkFor, type LinkProps } from "./Results";

// Waits this long after the last key press before searching.
const TYPING_DELAY_MS = 250;

// A plain left click. Others, like Ctrl or middle click, are left to the
// browser, which opens the link in a new tab.
function plainClick(event: MouseEvent): boolean {
  return event.button === 0 && !event.metaKey && !event.ctrlKey && !event.shiftKey && !event.altKey;
}

// Set in history entries for a file opened from the results, so "Back to
// results" can go back instead of adding another entry.
interface HistoryState {
  fromResults?: boolean;
}

export function App() {
  const [params, setParams] = useState<SearchParams>(() => readAddress(window.location.search).params);
  const [open, setOpen] = useState<OpenFile | null>(() => readAddress(window.location.search).open);
  // Whether the file view should scroll to open.line. Not when the line
  // was picked by clicking it, which would move the text under the mouse.
  const [jump, setJump] = useState(true);
  const [repos, setRepos] = useState<Repo[]>([]);
  const [reposError, setReposError] = useState("");
  const [response, setResponse] = useState<SearchResponse | null>(null);
  const [searching, setSearching] = useState(false);
  // The search the results are for, so a checkbox changed later doesn't
  // change how old results are described.
  const [shownParams, setShownParams] = useState<SearchParams>(params);
  const running = useRef<AbortController | null>(null);
  // Where the results were scrolled to when a file was opened.
  const resultsScroll = useRef(0);
  // The latest params and open file, for the link callbacks below, which
  // stay the same between renders so result lines needn't render again.
  const paramsRef = useRef(params);
  paramsRef.current = params;
  const openRef = useRef(open);
  openRef.current = open;

  useEffect(() => {
    listRepos().then(setRepos, (error: unknown) => setReposError(String(error)));
  }, []);

  // The page scrolls itself: to a file's line, or back to where the
  // results were.
  useEffect(() => {
    window.history.scrollRestoration = "manual";
  }, []);

  // The back and forward buttons: show what the address says.
  useEffect(() => {
    const onPopState = () => {
      const address = readAddress(window.location.search);
      if (openRef.current === null && address.open !== null) {
        resultsScroll.current = window.scrollY; // leaving the results, like with Forward
      }
      setOpen(address.open);
      setJump(true);
      setParams((current) => (sameParams(current, address.params) ? current : address.params));
    };
    window.addEventListener("popstate", onPopState);
    return () => window.removeEventListener("popstate", onPopState);
  }, []);

  // Keep the address up to date, without adding history entries.
  useEffect(() => {
    window.history.replaceState(window.history.state, "", addressFor(params, open));
  }, [params, open]);

  // Back at the results: scroll to where they were.
  useLayoutEffect(() => {
    if (open === null) {
      window.scrollTo(0, resultsScroll.current);
    }
  }, [open]);

  // Searches again shortly after anything changes. A new search cancels
  // the one before it, which also stops it on the server.
  useEffect(() => {
    if (params.query.trim() === "") {
      running.current?.abort();
      setResponse(null);
      setSearching(false);
      return;
    }
    const timer = window.setTimeout(() => {
      running.current?.abort();
      const controller = new AbortController();
      running.current = controller;
      setSearching(true);
      search(params, controller.signal).then(
        (result) => {
          setResponse(result);
          setShownParams(params);
          setSearching(false);
        },
        (error: unknown) => {
          if (!controller.signal.aborted) {
            setResponse({ files: [], status: "error", error: `could not reach the server: ${String(error)}` });
            setSearching(false);
          }
        },
      );
    }, TYPING_DELAY_MS);
    return () => window.clearTimeout(timer);
  }, [params]);

  // A link to a file. From the results it adds a history entry, so Back
  // returns to them; inside a file, picking another line replaces it. The
  // href is made from the search when the line renders; a click uses the
  // search as it is then.
  const linkTo = useCallback((target: OpenFile, replace: boolean): LinkProps => {
    return {
      href: addressFor(paramsRef.current, target),
      onClick: (event) => {
        if (!plainClick(event)) {
          return;
        }
        event.preventDefault();
        const address = addressFor(paramsRef.current, target);
        if (replace) {
          window.history.replaceState(window.history.state, "", address);
          setJump(false);
        } else {
          resultsScroll.current = window.scrollY;
          const state: HistoryState = { fromResults: true };
          window.history.pushState(state, "", address);
          setJump(true);
        }
        setOpen(target);
      },
    };
  }, []);
  const resultLink = useCallback<LinkFor>((target) => linkTo(target, false), [linkTo]);
  const lineLink = useCallback<LinkFor>((target) => linkTo(target, true), [linkTo]);

  const back: LinkProps = {
    href: addressFor(params, null),
    onClick: (event) => {
      if (!plainClick(event)) {
        return;
      }
      event.preventDefault();
      if ((window.history.state as HistoryState | null)?.fromResults === true) {
        window.history.back(); // popstate closes the file
      } else {
        resultsScroll.current = 0;
        window.history.pushState(null, "", addressFor(params, null));
        setOpen(null);
      }
    },
  };

  // Changing the search closes the open file, to show the new results.
  const update = (change: Partial<SearchParams>) => {
    setParams({ ...params, ...change });
    if (open !== null) {
      resultsScroll.current = 0;
      setOpen(null);
    }
  };

  return (
    <div className="app">
      <header>
        <h1>code-search</h1>
        <form className="search-form" role="search" onSubmit={(event) => event.preventDefault()}>
          <input
            className="query"
            type="search"
            aria-label="Search"
            placeholder="Search, like: auth lang:go -path:_test"
            value={params.query}
            onChange={(event) => update({ query: event.target.value })}
            autoFocus
            spellCheck={false}
            autoComplete="off"
          />
          <div className="filters">
            <label>
              Repo{" "}
              <select value={params.repo} onChange={(event) => update({ repo: event.target.value })}>
                <option value="">All repos</option>
                {repos.map((repo) => (
                  <option key={repo.name} value={repo.name}>
                    {repo.name}
                  </option>
                ))}
              </select>
            </label>
            <label>
              <input
                type="checkbox"
                checked={params.caseSensitive}
                onChange={(event) => update({ caseSensitive: event.target.checked })}
              />{" "}
              Match case
            </label>
            <label>
              <input
                type="checkbox"
                checked={params.filesOnly}
                onChange={(event) => update({ filesOnly: event.target.checked })}
              />{" "}
              Files only
            </label>
            <label>
              Context{" "}
              <CountInput value={params.context} max={20} onChange={(context) => update({ context })} />
            </label>
            <label>
              Limit{" "}
              <CountInput value={params.maxResults} onChange={(maxResults) => update({ maxResults })} />
            </label>
            {searching && <span className="searching">Searching…</span>}
          </div>
        </form>
        {reposError !== "" && <p className="error">Could not list the repos: {reposError}</p>}
      </header>
      <main>
        {open !== null && <FileView open={open} jump={jump} linkFor={lineLink} back={back} />}
        {/* Kept while a file is open, only hidden, so going back is instant. */}
        <div hidden={open !== null}>
          {response === null ? (
            <Help />
          ) : (
            <Results response={response} filesOnly={shownParams.filesOnly} linkFor={resultLink} />
          )}
        </div>
      </main>
    </div>
  );
}

// A whole number, 0 or more. What's typed is kept as text, and only a
// whole number is passed on: clearing the box to type another number
// mustn't search with 0, which for the limit means no limit at all.
function CountInput({ value, max, onChange }: { value: number; max?: number; onChange: (value: number) => void }) {
  const [text, setText] = useState(String(value));
  // Follow changes from elsewhere, like the back button.
  useEffect(() => {
    setText((current) => (countIn(current) === value ? current : String(value)));
  }, [value]);
  return (
    <input
      type="number"
      min={0}
      max={max}
      value={text}
      onChange={(event) => {
        setText(event.target.value);
        const count = countIn(event.target.value);
        if (count !== null && count !== value) {
          onChange(count);
        }
      }}
      onBlur={() => setText(String(value))}
    />
  );
}

function Help() {
  return (
    <div className="help">
      <p>Type to search every added repo. Some examples:</p>
      <ul>
        <li><code>foo bar</code> both words in a file</li>
        <li><code>foo or bar</code> either one</li>
        <li><code>foo -bar</code> files with foo but not bar</li>
        <li><code>"foo bar"</code> the exact text, spaces and all</li>
        <li><code>/fo+ (bar|baz)/</code> a regex</li>
        <li><code>repo:x path:src lang:go case:yes</code> filters</li>
      </ul>
      <p>A limit of 0 means no limit. Click a file or a line to open it.</p>
    </div>
  );
}
