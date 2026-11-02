// @ts-check
// Public URL API over URLImpl::{parse,set,parseQuery,serializeQuery}.
// Only Ada parsing/encoding crosses into C++; JS owns all object behavior.

import { urlImpl } from "amj:url_impl";

/**
 * @typedef {import("amj:url_impl").URLParts} URLParts
 */
/**
 * @typedef {import("amj:url_impl").URLComponent} URLComponent
 */
/**
 * @typedef {{pairs: [string, string][], changed: ((encoded: string) => void) | null}} ParamsState
 */
/**
 * @type {WeakMap<URLSearchParams, ParamsState>}
 */
const paramsStates = new WeakMap();
const pairLimit = 65536;
const inputLimit = 1048576;
/**
 * @param {unknown} value
 * @returns {string}
 */
const scalarString = (value) => {
  if (typeof value === "symbol") {
    throw new TypeError("Cannot convert a Symbol to a URL string");
  }
  // Unicode matching sees paired surrogates as one non-surrogate code point.
  return String(value).replace(/[\uD800-\uDFFF]/gu, "\uFFFD");
};
/**
 * @param {string} text
 * @returns {number}
 */
const byteLength = (text) => {
  let bytes = 0;
  for (const character of text) {
    const point = character.codePointAt(0);
    if (point === undefined) {
      throw new TypeError("Unexpected empty string character");
    }
    if (point < 0x80) {
      bytes++;
    } else if (point < 0x800) {
      bytes += 2;
    } else if (point < 0x10000) {
      bytes += 3;
    } else {
      bytes += 4;
    }
  }
  return bytes;
};
export class URLSearchParams {
  /**
   * @type {ParamsState}
   */
  #state = { pairs: [], changed: null };
  /**
   * @param {unknown} [input]
   */
  constructor(input) {
    /**
     * @type {[string, string][]}
     */
    let pairs = [];
    let bytes = 0;
    if (input !== undefined && input !== null) {
      if (typeof input === "object" || typeof input === "function") {
        // Dynamic JS records/iterators are checked here; yielded values
        // remain unknown until scalarString converts them.
        const properties =
          /**
           * @type {Record<PropertyKey, unknown>}
           */ (input);
        const iterator = properties[Symbol.iterator];
        if (iterator !== undefined && iterator !== null) {
          if (typeof iterator !== "function") {
            throw new TypeError("Expected an iterable");
          }
          const iterable = {
            [Symbol.iterator]: () =>
              /**
               * @type {() => Iterator<unknown>}
               */ (iterator).call(input),
          };
          for (const pair of iterable) {
            if (pairs.length >= pairLimit) {
              throw new RangeError("URL query exceeds 65536 pairs");
            }
            if (pair === null || (typeof pair !== "object" && typeof pair !== "function")) {
              throw new TypeError("Expected iterable name/value pairs");
            }
            /**
             * @type {string[]}
             */
            const fields = [];
            // for...of performs runtime iterator-protocol validation.
            for (const field of /**
             * @type {Iterable<unknown>}
             */ (pair)) {
              if (fields.length === 2) {
                throw new TypeError("Expected exactly two fields per pair");
              }
              fields.push(scalarString(field));
            }
            const [name, value] = fields;
            if (fields.length !== 2 || name === undefined || value === undefined) {
              throw new TypeError("Expected exactly two fields per pair");
            }
            bytes += byteLength(name) + byteLength(value);
            if (bytes > inputLimit) {
              throw new RangeError("URL query exceeds 1 MiB");
            }
            pairs.push([name, value]);
          }
        } else {
          for (const name of Object.keys(input)) {
            if (pairs.length >= pairLimit) {
              throw new RangeError("URL query exceeds 65536 pairs");
            }
            /**
             * @type {[string, string]}
             */
            const fields = [scalarString(name), scalarString(properties[name])];
            bytes += byteLength(fields[0]) + byteLength(fields[1]);
            if (bytes > inputLimit) {
              throw new RangeError("URL query exceeds 1 MiB");
            }
            pairs.push(fields);
          }
        }
      } else {
        pairs = urlImpl.parseQuery(scalarString(input));
      }
    }
    urlImpl.serializeQuery(pairs);
    this.#state.pairs = pairs;
    paramsStates.set(this, this.#state);
  }
  /**
   * @param {[string, string][]} pairs
   * @returns {void}
   */
  #commit(pairs) {
    const encoded = urlImpl.serializeQuery(pairs);
    if (this.#state.changed !== null) {
      this.#state.changed(encoded);
    }
    this.#state.pairs = pairs;
  }
  get size() {
    return this.#state.pairs.length;
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
    const nameText = scalarString(name);
    const valueText = scalarString(value);
    this.#commit([...this.#state.pairs, [nameText, valueText]]);
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
    name = scalarString(name);
    const matchValue = value !== undefined;
    if (matchValue) {
      value = scalarString(value);
    }
    this.#commit(this.#state.pairs.filter((pair) => pair[0] !== name || (matchValue && pair[1] !== value)));
  }
  /**
   * @param {unknown} name
   * @returns {string | null}
   */
  get(name) {
    if (arguments.length < 1) {
      throw new TypeError("get requires a name");
    }
    name = scalarString(name);
    for (const pair of this.#state.pairs) {
      if (pair[0] === name) {
        return pair[1];
      }
    }
    return null;
  }
  /**
   * @param {unknown} name
   * @returns {string[]}
   */
  getAll(name) {
    if (arguments.length < 1) {
      throw new TypeError("getAll requires a name");
    }
    name = scalarString(name);
    const values = [];
    for (const pair of this.#state.pairs) {
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
    name = scalarString(name);
    const matchValue = value !== undefined;
    if (matchValue) {
      value = scalarString(value);
    }
    return this.#state.pairs.some((pair) => pair[0] === name && (!matchValue || pair[1] === value));
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
    const nameText = scalarString(name);
    const valueText = scalarString(value);
    /**
     * @type {[string, string][]}
     */
    const pairs = [];
    let found = false;
    for (const pair of this.#state.pairs) {
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
    const pairs = this.#state.pairs.slice();
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
    return urlImpl.serializeQuery(this.#state.pairs);
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
    while (index < this.#state.pairs.length) {
      const pair = this.#state.pairs[index++];
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
    while (index < this.#state.pairs.length) {
      const pair = this.#state.pairs[index++];
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
  /**
   * @type {URLParts}
   */
  #parts;
  /**
   * @type {URLSearchParams}
   */
  #searchParams;
  /**
   * @param {unknown} input
   * @param {unknown} [base]
   */
  constructor(input, base) {
    if (arguments.length < 1) {
      throw new TypeError("URL requires an input");
    }
    const inputText = scalarString(input);
    const baseText = base === undefined ? null : scalarString(base);
    const parts = urlImpl.parse(inputText, baseText);
    if (parts === null) {
      throw new TypeError("Invalid URL");
    }
    this.#parts = parts;
    this.#searchParams = new URLSearchParams(parts.search);
    const paramsState = paramsStates.get(this.#searchParams);
    if (paramsState === undefined) {
      throw new TypeError("Missing URL query state");
    }
    paramsState.changed = (encoded) => {
      const updated = urlImpl.set(this.#parts.href, "search", encoded);
      if (updated === null) {
        throw new TypeError("Invalid URL query");
      }
      this.#parts = updated;
    };
  }
  /**
   * @param {URLComponent} component
   * @param {unknown} value
   * @returns {void}
   */
  #set(component, value) {
    const valueText = scalarString(value); // Coercion can mutate the URL; read href afterward.
    const updated = urlImpl.set(this.#parts.href, component, valueText);
    if (updated === null) {
      if (component === "href") {
        throw new TypeError("Invalid URL");
      }
      return;
    }
    if (component === "href" || component === "search") {
      const pairs = urlImpl.parseQuery(updated.search);
      urlImpl.serializeQuery(pairs);
      const paramsState = paramsStates.get(this.#searchParams);
      if (paramsState === undefined) {
        throw new TypeError("Missing URL query state");
      }
      paramsState.pairs = pairs;
    }
    this.#parts = updated;
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
    const inputText = scalarString(input);
    const baseText = base === undefined ? null : scalarString(base);
    return urlImpl.parse(inputText, baseText) !== null;
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
    const inputText = scalarString(input);
    const baseText = base === undefined ? null : scalarString(base);
    const parts = urlImpl.parse(inputText, baseText);
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
    return this.#searchParams;
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
