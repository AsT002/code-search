import { useEffect, useRef, useState } from "react";

import { listRepos, search, type Repo, type SearchParams, type SearchResponse } from "./api";
import { Results } from "./Results";

const DEFAULTS: SearchParams = {
  query: "",
  repo: "",
  caseSensitive: false,
  filesOnly: false,
  context: 0,
  maxResults: 200,
};

// Waits this long after the last key press before searching.
const TYPING_DELAY_MS = 250;

// The search in the page's address, so a reload or a shared link shows it.
function paramsFromAddress(): SearchParams {
  const address = new URLSearchParams(window.location.search);
  const count = (name: string, fallback: number) => {
    const value = Number(address.get(name));
    return Number.isInteger(value) && value >= 0 && address.has(name) ? value : fallback;
  };
  return {
    query: address.get("q") ?? "",
    repo: address.get("repo") ?? "",
    caseSensitive: address.get("case") === "yes",
    filesOnly: address.get("files") === "yes",
    context: count("context", DEFAULTS.context),
    maxResults: count("max", DEFAULTS.maxResults),
  };
}

function saveToAddress(params: SearchParams) {
  const address = new URLSearchParams();
  if (params.query !== "") address.set("q", params.query);
  if (params.repo !== "") address.set("repo", params.repo);
  if (params.caseSensitive) address.set("case", "yes");
  if (params.filesOnly) address.set("files", "yes");
  if (params.context !== DEFAULTS.context) address.set("context", String(params.context));
  if (params.maxResults !== DEFAULTS.maxResults) address.set("max", String(params.maxResults));
  const text = address.toString();
  window.history.replaceState(null, "", text === "" ? window.location.pathname : `?${text}`);
}

export function App() {
  const [params, setParams] = useState<SearchParams>(paramsFromAddress);
  const [repos, setRepos] = useState<Repo[]>([]);
  const [reposError, setReposError] = useState("");
  const [response, setResponse] = useState<SearchResponse | null>(null);
  const [searching, setSearching] = useState(false);
  // The search the results are for, so a checkbox changed later doesn't
  // change how old results are described.
  const [shownParams, setShownParams] = useState<SearchParams>(params);
  const running = useRef<AbortController | null>(null);

  useEffect(() => {
    listRepos().then(setRepos, (error: unknown) => setReposError(String(error)));
  }, []);

  // Searches again shortly after anything changes. A new search cancels
  // the one before it, which also stops it on the server.
  useEffect(() => {
    saveToAddress(params);
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

  const update = (change: Partial<SearchParams>) => setParams({ ...params, ...change });

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
              <input
                type="number"
                min={0}
                max={20}
                value={params.context}
                onChange={(event) => update({ context: Math.max(0, Math.trunc(Number(event.target.value))) })}
              />
            </label>
            <label>
              Limit{" "}
              <input
                type="number"
                min={0}
                value={params.maxResults}
                onChange={(event) => update({ maxResults: Math.max(0, Math.trunc(Number(event.target.value))) })}
              />
            </label>
            {searching && <span className="searching">Searching…</span>}
          </div>
        </form>
        {reposError !== "" && <p className="error">Could not list the repos: {reposError}</p>}
      </header>
      <main>
        {response === null ? (
          <Help />
        ) : (
          <Results response={response} filesOnly={shownParams.filesOnly} />
        )}
      </main>
    </div>
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
      <p>A limit of 0 means no limit.</p>
    </div>
  );
}
