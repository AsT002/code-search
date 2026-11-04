// zoekt-all: zoekt's own CLI (cmd/zoekt), but with its match limits raised,
// so it returns every match like grep, csearch and code-search do. The
// stock CLI leaves SearchOptions empty, and zoekt then stops after 100,000
// matches per shard (SearchOptions.SetDefaults). Prints repo/path, so
// results can be compared across repos. For benchmarks/vs_indexed.py.
package main

import (
	"bufio"
	"bytes"
	"context"
	"flag"
	"fmt"
	"os"
	"strings"

	"github.com/sourcegraph/zoekt"
	"github.com/sourcegraph/zoekt/query"
	"github.com/sourcegraph/zoekt/search"
)

// fail reports an error and exits with 2: grep's 1 means "nothing found",
// which log.Fatal's 1 would look like to a benchmark.
func fail(err error) {
	fmt.Fprintln(os.Stderr, "zoekt-all:", err)
	os.Exit(2)
}

func main() {
	index := flag.String("index_dir", "", "search for index files in directory")
	list := flag.Bool("l", false, "print matching filenames only")
	flag.Parse()
	searcher, err := search.NewDirectorySearcher(*index)
	if err != nil {
		fail(err)
	}
	q, err := query.Parse(strings.Join(flag.Args(), " "))
	if err != nil {
		fail(err)
	}
	q = query.Simplify(query.Map(q, query.ExpandFileContent))
	const many = 1 << 30
	opts := zoekt.SearchOptions{ShardMaxMatchCount: many, TotalMaxMatchCount: many}
	res, err := searcher.Search(context.Background(), q, &opts)
	if err != nil {
		fail(err)
	}
	out := bufio.NewWriterSize(os.Stdout, 1<<16)
	defer out.Flush()
	for _, f := range res.Files {
		if *list {
			fmt.Fprintf(out, "%s/%s\n", f.Repository, f.FileName)
			continue
		}
		for _, m := range f.LineMatches {
			fmt.Fprintf(out, "%s/%s:%d:%s\n", f.Repository, f.FileName, m.LineNumber, bytes.TrimSuffix(m.Line, []byte{'\n'}))
		}
	}
}
