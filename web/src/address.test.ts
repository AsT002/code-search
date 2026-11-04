import { describe, expect, test } from "vitest";

import { DEFAULTS, addressFor, readAddress } from "./address";

describe("address", () => {
  test("a search and an open file read back the same", () => {
    const params = { ...DEFAULTS, query: "a&b #c", repo: "redis", context: 2 };
    const open = { repo: "redis", path: "src/a b+c.c", line: 12 };
    expect(readAddress(addressFor(params, open))).toEqual({ params, open });
    expect(readAddress(addressFor(params, { ...open, line: null }))).toEqual({
      params,
      open: { ...open, line: null },
    });
    expect(readAddress(addressFor(params, null))).toEqual({ params, open: null });
  });

  test("defaults stay out of the address", () => {
    expect(addressFor(DEFAULTS, null)).toBe("./");
    expect(addressFor({ ...DEFAULTS, query: "x" }, null)).toBe("?q=x");
  });

  test("a bad line or a file without its repo is ignored", () => {
    expect(readAddress("?file_repo=r&file=a.c&line=-3").open).toEqual({ repo: "r", path: "a.c", line: null });
    expect(readAddress("?file_repo=r&file=a.c&line=0").open?.line).toBeNull();
    expect(readAddress("?file=a.c&line=3").open).toBeNull();
    expect(readAddress("?file_repo=&file=a.c").open).toBeNull();
  });
});
