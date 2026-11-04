// @ts-check
// Public URL API over the native URL and query-string functions.
// Only Ada parsing/encoding crosses into C++; JS owns all object behavior.

import { queryStringParse, queryStringify, urlParse, urlWithComponent } from "amj:url_impl";

/**
 * @typedef {import("amj:url_impl").URLParts} URLParts
 */

/**
 * @type {(urlSearchParams: URLSearchParams) => string}
 */
let getQueryString;

/**
 * @type {(urlSearchParams: URLSearchParams, urlParts: URLParts, replaceEntries: boolean) => string}
 */
let setRawQuery;

export class URLSearchParams {
  /**
   * @type {Array<[string, string]>}
   */
  #entries = [];

  #queryString = "";

  static {
    getQueryString = function (urlSearchParams) {
      return urlSearchParams.#queryString;
    };
    setRawQuery = (urlSearchParams, urlParts, replaceEntries) => {
      // Unlike parts.search, the href distinguishes an empty '?' from no query.
      const hashIndex = urlParts.href.indexOf("#");
      const queryEnd = hashIndex === -1 ? urlParts.href.length : hashIndex;
      const queryIndex = urlParts.href.indexOf("?");
      const hasQuery = queryIndex !== -1 && queryIndex < queryEnd;
      const rawQuery = hasQuery ? urlParts.href.slice(queryIndex, queryEnd) : "";
      let entries = urlSearchParams.#entries;
      if (replaceEntries) {
        entries = queryStringParse(rawQuery);
        queryStringify(entries);
      }
      urlSearchParams.#entries = entries;
      urlSearchParams.#queryString = rawQuery;
      // Replace query contents with a bare delimiter; URL installs the current query.
      return hasQuery ? urlParts.href.slice(0, queryIndex + 1) + urlParts.href.slice(queryEnd) : urlParts.href;
    };
  }
  /**
   * @param {unknown} [options]
   */
  constructor(options) {
    if (options == null) {
      return;
    }
    /**
     * @type {Array<[string, string]>}
     */
    let entries = [];
    if (typeof options === "object" || typeof options === "function") {
      // Dynamic JS records/iterators are checked here; yielded values
      // remain unknown until scalarString converts them.
      const properties =
        /**
         * @type {Record<PropertyKey, unknown>}
         */ (options);
      const iterator = properties[Symbol.iterator];
      if (iterator !== undefined && iterator !== null) {
        if (typeof iterator !== "function") {
          throw new TypeError("Expected an iterable");
        }
        const iterable = {
          [Symbol.iterator]: () =>
            /**
             * @type {() => Iterator<unknown>}
             */ (iterator).call(options),
        };
        for (const pair of iterable) {
          if (entries.length >= 65536) {
            throw new RangeError("URL query exceeds 65536 pairs");
          }
          if (pair === null || (typeof pair !== "object" && typeof pair !== "function")) {
            throw new TypeError("Expected iterable name/value pairs");
          }
          /**
           * @type {Array<string>}
           */
          const fields = [];
          // for...of performs runtime iterator-protocol validation.
          for (const field of /**
           * @type {Iterable<unknown>}
           */ (pair)) {
            if (fields.length === 2) {
              throw new TypeError("Expected exactly two fields per pair");
            }
            fields.push(stringify(field));
          }
          const [name, value] = fields;
          if (fields.length !== 2 || name === undefined || value === undefined) {
            throw new TypeError("Expected exactly two fields per pair");
          }
          entries.push([name, value]);
        }
      } else {
        for (const name of Object.keys(options)) {
          if (entries.length >= 65536) {
            throw new RangeError("URL query exceeds 65536 pairs");
          }
          /**
           * @type {[string, string]}
           */
          const fields = [stringify(name), stringify(properties[name])];
          entries.push(fields);
        }
      }
    } else {
      entries = queryStringParse(stringify(options));
    }
    queryStringify(entries);
    this.#entries = entries;
  }
  /**
   * @param {Array<[string, string]>} pairs
   * @returns {void}
   */
  #commit(pairs) {
    const encoded = queryStringify(pairs);
    const rawQuery = encoded === "" ? "" : `?${encoded}`;
    this.#entries = pairs;
    this.#queryString = rawQuery;
  }
  get size() {
    return this.#entries.length;
  }
  /**
   * @param {unknown} name
   * @param {unknown} value
   * @returns {void}
   */
  append(name, value) {
    if (arguments.length < 2) {
      throw new TypeError("append requires two arguments");
    }
    const nameText = stringify(name);
    const valueText = stringify(value);
    this.#commit([...this.#entries, [nameText, valueText]]);
  }
  /**
   * @param {unknown} name
   * @param {unknown} [value]
   * @returns {void}
   */
  delete(name, value) {
    if (arguments.length < 1) {
      throw new TypeError("delete requires a name");
    }
    name = stringify(name);
    const matchValue = value !== undefined;
    if (matchValue) {
      value = stringify(value);
    }
    this.#commit(this.#entries.filter((pair) => pair[0] !== name || (matchValue && pair[1] !== value)));
  }
  /**
   * @param {unknown} name
   * @returns {string | null}
   */
  get(name) {
    if (arguments.length < 1) {
      throw new TypeError("get requires a name");
    }
    name = stringify(name);
    for (const pair of this.#entries) {
      if (pair[0] === name) {
        return pair[1];
      }
    }
    return null;
  }
  /**
   * @param {unknown} name
   * @returns {Array<string>}
   */
  getAll(name) {
    if (arguments.length < 1) {
      throw new TypeError("getAll requires a name");
    }
    name = stringify(name);
    const values = [];
    for (const pair of this.#entries) {
      if (pair[0] === name) {
        values.push(pair[1]);
      }
    }
    return values;
  }
  /**
   * @param {unknown} name
   * @param {unknown} [value]
   * @returns {boolean}
   */
  has(name, value) {
    if (arguments.length < 1) {
      throw new TypeError("has requires a name");
    }
    name = stringify(name);
    const matchValue = value !== undefined;
    if (matchValue) {
      value = stringify(value);
    }
    return this.#entries.some((pair) => pair[0] === name && (!matchValue || pair[1] === value));
  }
  /**
   * @param {unknown} name
   * @param {unknown} value
   * @returns {void}
   */
  set(name, value) {
    if (arguments.length < 2) {
      throw new TypeError("set requires two arguments");
    }
    const nameText = stringify(name);
    const valueText = stringify(value);
    /**
     * @type {Array<[string, string]>}
     */
    const pairs = [];
    let found = false;
    for (const pair of this.#entries) {
      if (pair[0] !== nameText) {
        pairs.push(pair);
      } else if (!found) {
        pairs.push([nameText, valueText]);
        found = true;
      }
    }
    if (!found) {
      pairs.push([nameText, valueText]);
    }
    this.#commit(pairs);
  }
  sort() {
    const pairs = this.#entries.slice();
    pairs.sort((left, right) => {
      if (left[0] < right[0]) {
        return -1;
      }
      if (left[0] > right[0]) {
        return 1;
      }
      return 0;
    });
    this.#commit(pairs);
  }
  toString() {
    return queryStringify(this.#entries);
  }
  /**
   * @param {(this: unknown, value: string, name: string, params: URLSearchParams) => void} callback
   * @param {unknown} [thisArg]
   * @returns {void}
   */
  forEach(callback, thisArg) {
    if (typeof callback !== "function") {
      throw new TypeError("Expected a callback");
    }
    let index = 0;
    while (index < this.#entries.length) {
      const pair = this.#entries[index++];
      if (pair === undefined) {
        throw new RangeError("Invalid query pair index");
      }
      callback.call(thisArg, pair[1], pair[0], this);
    }
  }
  /**
   * @returns {Generator<[string, string], void, unknown>}
   */
  *entries() {
    let index = 0;
    while (index < this.#entries.length) {
      const pair = this.#entries[index++];
      if (pair === undefined) {
        throw new RangeError("Invalid query pair index");
      }
      yield [pair[0], pair[1]];
    }
  }
  /**
   * @returns {Generator<string, void, unknown>}
   */
  *keys() {
    for (const [name] of this.entries()) {
      yield name;
    }
  }
  /**
   * @returns {Generator<string, void, unknown>}
   */
  *values() {
    for (const [, value] of this.entries()) {
      yield value;
    }
  }
  [Symbol.iterator]() {
    return this.entries();
  }
  get [Symbol.toStringTag]() {
    return "URLSearchParams";
  }
}
Object.defineProperty(URLSearchParams.prototype, Symbol.iterator, {
  value: URLSearchParams.prototype.entries,
  writable: true,
  configurable: true,
});
export class URL {
  // URL serialization without query contents; URLSearchParams owns the query.
  #hrefWithoutQuery = "";

  /**
   * @returns {URLParts}
   */
  get #parts() {
    const urlParts = urlWithComponent(this.#hrefWithoutQuery, "search", getQueryString(this.#urlSearchParams));
    if (urlParts === null) {
      throw new TypeError("Invalid URL");
    }
    return urlParts;
  }
  /**
   * @type {URLSearchParams}
   */
  #urlSearchParams;
  /**
   * @param {unknown} input
   * @param {unknown} [base]
   */
  constructor(input, base) {
    if (arguments.length < 1) {
      throw new TypeError("URL requires an input");
    }
    const inputText = stringify(input);
    const baseText = base === undefined ? undefined : stringify(base);
    const parts = urlParse(inputText, baseText);
    if (parts === null) {
      throw new TypeError("Invalid URL");
    }
    this.#urlSearchParams = new URLSearchParams(parts.search);
    this.#hrefWithoutQuery = setRawQuery(this.#urlSearchParams, parts, false);
  }
  /**
   * @param {string} component
   * @param {unknown} value
   * @returns {void}
   */
  #set(component, value) {
    const valueText = stringify(value); // Coercion can mutate the URL; read href afterward.
    const updated = urlWithComponent(this.#parts.href, component, valueText);
    if (updated === null) {
      if (component === "href") {
        throw new TypeError("Invalid URL");
      }
      return;
    }
    this.#hrefWithoutQuery = setRawQuery(
      this.#urlSearchParams,
      updated,
      component === "href" || component === "search",
    );
  }
  /**
   * @param {unknown} input
   * @param {unknown} [base]
   * @returns {boolean}
   */
  static canParse(input, base) {
    if (arguments.length < 1) {
      throw new TypeError("URL.canParse requires an input");
    }
    const inputText = stringify(input);
    const baseText = base === undefined ? undefined : stringify(base);
    return urlParse(inputText, baseText) !== null;
  }
  /**
   * @param {unknown} input
   * @param {unknown} [base]
   * @returns {URL | null}
   */
  static parse(input, base) {
    if (arguments.length < 1) {
      throw new TypeError("URL.parse requires an input");
    }
    const inputText = stringify(input);
    const baseText = base === undefined ? undefined : stringify(base);
    const parts = urlParse(inputText, baseText);
    if (parts === null) {
      return null;
    }
    return new URL(parts.href);
  }
  get href() {
    return this.#parts.href;
  }
  set href(value) {
    this.#set("href", value);
  }
  get origin() {
    return this.#parts.origin;
  }
  get protocol() {
    return this.#parts.protocol;
  }
  set protocol(value) {
    this.#set("protocol", value);
  }
  get username() {
    return this.#parts.username;
  }
  set username(value) {
    this.#set("username", value);
  }
  get password() {
    return this.#parts.password;
  }
  set password(value) {
    this.#set("password", value);
  }
  get host() {
    return this.#parts.host;
  }
  set host(value) {
    this.#set("host", value);
  }
  get hostname() {
    return this.#parts.hostname;
  }
  set hostname(value) {
    this.#set("hostname", value);
  }
  get port() {
    return this.#parts.port;
  }
  set port(value) {
    this.#set("port", value);
  }
  get pathname() {
    return this.#parts.pathname;
  }
  set pathname(value) {
    this.#set("pathname", value);
  }
  get search() {
    return this.#parts.search;
  }
  set search(value) {
    this.#set("search", value);
  }
  get hash() {
    return this.#parts.hash;
  }
  set hash(value) {
    this.#set("hash", value);
  }
  get searchParams() {
    return this.#urlSearchParams;
  }
  toString() {
    return this.#parts.href;
  }
  toJSON() {
    return this.#parts.href;
  }
  get [Symbol.toStringTag]() {
    return "URL";
  }
}

/**
 * @param {unknown} value
 * @returns {string}
 */
function stringify(value) {
  if (typeof value === "symbol") {
    throw new TypeError("Cannot convert a Symbol value to a string");
  }
  // Unicode matching sees paired surrogates as one non-surrogate code point.
  return String(value).replace(/[\uD800-\uDFFF]/gu, "\uFFFD");
}
