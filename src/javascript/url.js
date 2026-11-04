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
    if (options === null || options === undefined) {
      return;
    }
    if (typeof options !== "object" && typeof options !== "function") {
      this.#entries = queryStringParse(stringify(options));
      return;
    }
    const entriesIteratorFunction = Reflect.get(options, Symbol.iterator);
    if (entriesIteratorFunction === null || entriesIteratorFunction === undefined) {
      for (const name of Object.keys(options)) {
        if (this.#entries.length > 65535) {
          throw new RangeError("queryString has too many entries");
        }
        this.#entries.push([stringify(name), stringify(Reflect.get(options, name))]);
      }
      return;
    }
    if (typeof entriesIteratorFunction !== "function") {
      throw new TypeError("options is not an iterable");
    }
    const entriesIterable = {
      [Symbol.iterator]: function () {
        return Reflect.apply(entriesIteratorFunction, options, arguments);
      },
    };
    let i = -1;
    for (const entry of entriesIterable) {
      i++;
      if (this.#entries.length > 65535) {
        throw new RangeError("queryString has too many entries");
      }
      if (entry === null) {
        throw new TypeError(`entry #${i + 1} is null`);
      }
      if (typeof entry !== "object" && typeof entry !== "function") {
        throw new TypeError(`entry #${i + 1} is not an iterable`);
      }
      const entryIteratorFunction = Reflect.get(entry, Symbol.iterator);
      if (typeof entryIteratorFunction != "function") {
        throw new TypeError(`entry #${i + 1} is not an iterable`);
      }
      const entryIterable = {
        [Symbol.iterator]: function () {
          return Reflect.apply(entryIteratorFunction, entry, arguments);
        },
      };
      let j = -1;
      let name = "";
      let value = "";
      for (const field of entryIterable) {
        j++;
        if (j === 0) {
          name = stringify(field);
        } else if (j === 1) {
          value = stringify(field);
        } else {
          throw new TypeError(`entry #${i + 1} has more than two fields`);
        }
      }
      if (j < 1) {
        throw new TypeError(`entry #${i + 1} has less than two fields`);
      }
      this.#entries.push([name, value]);
    }
  }

  get [Symbol.toStringTag]() {
    return "URLSearchParams";
  }

  get size() {
    return this.#entries.length;
  }

  [Symbol.iterator]() {
    return this.entries();
  }

  /**
   * @param {unknown} name
   * @param {unknown} value
   * @returns {void}
   */
  append(name, value) {
    if (arguments.length < 2) {
      throw new TypeError(`URLSearchParams.append: requires at least 2 arguments (${arguments.length} provided)`);
    }
    const nameStr = stringify(name);
    const valueStr = stringify(value);
    const entries = Array.from(this.#entries);
    entries.push([nameStr, valueStr]);
    const queryString = queryStringify(entries);
    this.#entries = entries;
    this.#queryString = "?" + queryString;
  }

  /**
   * @param {unknown} name
   * @param {unknown} [value]
   * @returns {void}
   */
  delete(name, value) {
    if (arguments.length < 1) {
      throw new TypeError(`URLSearchParams.delete: requires at least 1 argument (${arguments.length} provided)`);
    }
    const nameStr = stringify(name);
    const valueStr = stringify(value);
    const entries = [];
    for (const entry of this.#entries) {
      if (entry[0] === nameStr) {
        if (value === undefined) {
          continue;
        }
        if (entry[1] === valueStr) {
          continue;
        }
      }
      entries.push(entry);
    }
    const queryString = queryStringify(entries);
    this.#entries = entries;
    this.#queryString = queryString === "" ? "" : "?" + queryString;
  }

  /**
   * @returns {Generator<[string, string], void, unknown>}
   */
  *entries() {
    for (let i = 0; i < this.#entries.length; i++) {
      const entry = this.#entries[i];
      if (entry === undefined) {
        throw new RangeError(`URLSearchParams.entries: index ${i} out of bounds (length = ${this.#entries.length})`);
      }
      yield [entry[0], entry[1]];
    }
  }

  /**
   * @param {(value: string, key: string, searchParams: URLSearchParams) => void} callback
   * @param {unknown} [thisArg]
   * @returns {void}
   */
  forEach(callback, thisArg) {
    if (arguments.length < 1) {
      throw new TypeError(`URLSearchParams.forEach: requires at least 1 argument (${arguments.length} provided)`);
    }
    if (typeof callback !== "function") {
      throw new TypeError("URLSearchParams.forEach: callback is not a function");
    }
    for (let i = 0; i < this.#entries.length; i++) {
      const entry = this.#entries[i];
      if (entry === undefined) {
        throw new RangeError(`URLSearchParams.entries: index ${i} out of bounds (length = ${this.#entries.length})`);
      }
      callback.call(thisArg, entry[1], entry[0], this);
    }
  }

  /**
   * @param {unknown} name
   * @returns {string | null}
   */
  get(name) {
    if (arguments.length < 1) {
      throw new TypeError(`URLSearchParams.get: requires at least 1 argument (${arguments.length} provided)`);
    }
    const nameStr = stringify(name);
    for (const entry of this.#entries) {
      if (entry[0] === nameStr) {
        return entry[1];
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
      throw new TypeError(`URLSearchParams.getAll: requires at least 1 argument (${arguments.length} provided)`);
    }
    const nameStr = stringify(name);
    const values = [];
    for (const entry of this.#entries) {
      if (entry[0] === nameStr) {
        values.push(entry[1]);
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
      throw new TypeError(`URLSearchParams.has: requires at least 1 argument (${arguments.length} provided)`);
    }
    const nameStr = stringify(name);
    const valueStr = stringify(value);
    for (const entry of this.#entries) {
      if (entry[0] !== nameStr) {
        continue;
      }
      if (value !== undefined) {
        if (entry[1] === valueStr) {
          continue;
        }
      }
      return true;
    }
    return false;
  }

  /**
   * @returns {Generator<string, void, unknown>}
   */
  *keys() {
    for (let i = 0; i < this.#entries.length; i++) {
      const entry = this.#entries[i];
      if (entry === undefined) {
        throw new RangeError(`URLSearchParams.keys: index ${i} out of bounds (length = ${this.#entries.length})`);
      }
      yield entry[0];
    }
  }

  /**
   * @param {unknown} name
   * @param {unknown} value
   * @returns {void}
   */
  set(name, value) {
    if (arguments.length < 2) {
      throw new TypeError(`URLSearchParams.set: requires at least 2 arguments (${arguments.length} provided)`);
    }
    const nameStr = stringify(name);
    const valueStr = stringify(value);
    /**
     * @type {Array<[string, string]>}
     */
    const pairs = [];
    let found = false;
    for (const pair of this.#entries) {
      if (pair[0] !== nameStr) {
        pairs.push(pair);
      } else if (!found) {
        pairs.push([nameStr, valueStr]);
        found = true;
      }
    }
    if (!found) {
      pairs.push([nameStr, valueStr]);
    }
    this.#commit(pairs);
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
   * @returns {Generator<string, void, unknown>}
   */
  *values() {
    for (let i = 0; i < this.#entries.length; i++) {
      const entry = this.#entries[i];
      if (entry === undefined) {
        throw new RangeError(`URLSearchParams.values: index ${i} out of bounds (length = ${this.#entries.length})`);
      }
      yield entry[1];
    }
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
