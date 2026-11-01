// Public URL API over NativeURL::{parse,set,parseQuery,serializeQuery}.
// Only Ada parsing/encoding crosses into C++; all JS object behavior lives here.
export function installURL(target, nativeURL) {
    for (const name of ["parse", "set", "parseQuery", "serializeQuery"]) {
        if (!nativeURL || typeof nativeURL[name] !== "function") throw new TypeError(`Missing native URL ${name}`);
    }
    const paramsStates = new WeakMap();
    const pairLimit = 65536;
    const inputLimit = 1048576;
    const scalarString = (value) => {
        if (typeof value === "symbol") throw new TypeError("Cannot convert a Symbol to a URL string");
        // With Unicode matching, paired surrogates are one non-surrogate code point.
        return String(value).replace(/[\uD800-\uDFFF]/gu, "\uFFFD");
    };
    const byteLength = (text) => {
        let bytes = 0;
        for (const character of text) {
            const point = character.codePointAt(0);
            if (point < 0x80) bytes++;
            else if (point < 0x800) bytes += 2;
            else if (point < 0x10000) bytes += 3;
            else bytes += 4;
        }
        return bytes;
    };
    class URLSearchParams {
        #state = { pairs: [], changed: null };
        constructor(input) {
            let pairs = [];
            let bytes = 0;
            if (input !== undefined && input !== null) {
                if (typeof input === "object" || typeof input === "function") {
                    const iterator = input[Symbol.iterator];
                    if (iterator !== undefined && iterator !== null) {
                        if (typeof iterator !== "function") throw new TypeError("Expected an iterable");
                        const iterable = { [Symbol.iterator]: () => iterator.call(input) };
                        for (const pair of iterable) {
                            if (pairs.length >= pairLimit) throw new RangeError("URL query exceeds 65536 pairs");
                            if (pair === null || (typeof pair !== "object" && typeof pair !== "function")) throw new TypeError("Expected iterable name/value pairs");
                            const fields = [];
                            for (const field of pair) {
                                if (fields.length === 2) throw new TypeError("Expected exactly two fields per pair");
                                fields.push(scalarString(field));
                            }
                            if (fields.length !== 2) throw new TypeError("Expected exactly two fields per pair");
                            bytes += byteLength(fields[0]) + byteLength(fields[1]);
                            if (bytes > inputLimit) throw new RangeError("URL query exceeds 1 MiB");
                            pairs.push(fields);
                        }
                    } else {
                        for (const name of Object.keys(input)) {
                            if (pairs.length >= pairLimit) throw new RangeError("URL query exceeds 65536 pairs");
                            const fields = [scalarString(name), scalarString(input[name])];
                            bytes += byteLength(fields[0]) + byteLength(fields[1]);
                            if (bytes > inputLimit) throw new RangeError("URL query exceeds 1 MiB");
                            pairs.push(fields);
                        }
                    }
                } else {
                    pairs = nativeURL.parseQuery(scalarString(input));
                }
            }
            nativeURL.serializeQuery(pairs); // Validate encoded length before publishing state.
            this.#state.pairs = pairs;
            paramsStates.set(this, this.#state);
        }
        #commit(pairs) {
            const encoded = nativeURL.serializeQuery(pairs);
            if (this.#state.changed !== null) this.#state.changed(encoded);
            this.#state.pairs = pairs;
        }
        get size() { return this.#state.pairs.length; }
        append(name, value) {
            if (arguments.length < 2) throw new TypeError("append requires two arguments");
            name = scalarString(name);
            value = scalarString(value);
            this.#commit([...this.#state.pairs, [name, value]]);
        }
        delete(name, value) {
            if (arguments.length < 1) throw new TypeError("delete requires a name");
            name = scalarString(name);
            const matchValue = value !== undefined;
            if (matchValue) value = scalarString(value);
            this.#commit(this.#state.pairs.filter((pair) => pair[0] !== name || (matchValue && pair[1] !== value)));
        }
        get(name) {
            if (arguments.length < 1) throw new TypeError("get requires a name");
            name = scalarString(name);
            for (const pair of this.#state.pairs) if (pair[0] === name) return pair[1];
            return null;
        }
        getAll(name) {
            if (arguments.length < 1) throw new TypeError("getAll requires a name");
            name = scalarString(name);
            const values = [];
            for (const pair of this.#state.pairs) if (pair[0] === name) values.push(pair[1]);
            return values;
        }
        has(name, value) {
            if (arguments.length < 1) throw new TypeError("has requires a name");
            name = scalarString(name);
            const matchValue = value !== undefined;
            if (matchValue) value = scalarString(value);
            return this.#state.pairs.some((pair) => pair[0] === name && (!matchValue || pair[1] === value));
        }
        set(name, value) {
            if (arguments.length < 2) throw new TypeError("set requires two arguments");
            name = scalarString(name);
            value = scalarString(value);
            const pairs = [];
            let found = false;
            for (const pair of this.#state.pairs) {
                if (pair[0] !== name) pairs.push(pair);
                else if (!found) {
                    pairs.push([name, value]);
                    found = true;
                }
            }
            if (!found) pairs.push([name, value]);
            this.#commit(pairs);
        }
        sort() {
            const pairs = this.#state.pairs.slice();
            pairs.sort((left, right) => {
                if (left[0] < right[0]) return -1;
                if (left[0] > right[0]) return 1;
                return 0;
            });
            this.#commit(pairs);
        }
        toString() { return nativeURL.serializeQuery(this.#state.pairs); }
        forEach(callback, thisArg) {
            if (typeof callback !== "function") throw new TypeError("Expected a callback");
            let index = 0;
            while (index < this.#state.pairs.length) {
                const [name, value] = this.#state.pairs[index++];
                callback.call(thisArg, value, name, this);
            }
        }
        *entries() {
            let index = 0;
            while (index < this.#state.pairs.length) yield this.#state.pairs[index++].slice();
        }
        *keys() { for (const [name] of this.entries()) yield name; }
        *values() { for (const [, value] of this.entries()) yield value; }
        get [Symbol.toStringTag]() { return "URLSearchParams"; }
    }
    Object.defineProperty(URLSearchParams.prototype, Symbol.iterator, {
        value: URLSearchParams.prototype.entries, writable: true, configurable: true,
    });
    class URL {
        #parts;
        #searchParams;
        constructor(input, base) {
            if (arguments.length < 1) throw new TypeError("URL requires an input");
            input = scalarString(input);
            if (base === undefined) base = null;
            else base = scalarString(base);
            const parts = nativeURL.parse(input, base);
            if (parts === null) throw new TypeError("Invalid URL");
            this.#parts = parts;
            this.#searchParams = new URLSearchParams(parts.search);
            paramsStates.get(this.#searchParams).changed = (encoded) => {
                const updated = nativeURL.set(this.#parts.href, "search", encoded);
                if (updated === null) throw new TypeError("Invalid URL query");
                this.#parts = updated;
            };
        }
        #set(component, value) {
            value = scalarString(value); // Coercion can mutate this URL; read href afterward.
            const updated = nativeURL.set(this.#parts.href, component, value);
            if (updated === null) {
                if (component === "href") throw new TypeError("Invalid URL");
                return;
            }
            if (component === "href" || component === "search") {
                const pairs = nativeURL.parseQuery(updated.search);
                nativeURL.serializeQuery(pairs);
                paramsStates.get(this.#searchParams).pairs = pairs;
            }
            this.#parts = updated;
        }
        static canParse(input, base) {
            if (arguments.length < 1) throw new TypeError("URL.canParse requires an input");
            input = scalarString(input);
            if (base === undefined) base = null;
            else base = scalarString(base);
            return nativeURL.parse(input, base) !== null;
        }
        static parse(input, base) {
            if (arguments.length < 1) throw new TypeError("URL.parse requires an input");
            input = scalarString(input);
            if (base === undefined) base = null;
            else base = scalarString(base);
            const parts = nativeURL.parse(input, base);
            if (parts === null) return null;
            return new URL(parts.href);
        }
        get href() { return this.#parts.href; }
        set href(value) { this.#set("href", value); }
        get origin() { return this.#parts.origin; }
        get protocol() { return this.#parts.protocol; }
        set protocol(value) { this.#set("protocol", value); }
        get username() { return this.#parts.username; }
        set username(value) { this.#set("username", value); }
        get password() { return this.#parts.password; }
        set password(value) { this.#set("password", value); }
        get host() { return this.#parts.host; }
        set host(value) { this.#set("host", value); }
        get hostname() { return this.#parts.hostname; }
        set hostname(value) { this.#set("hostname", value); }
        get port() { return this.#parts.port; }
        set port(value) { this.#set("port", value); }
        get pathname() { return this.#parts.pathname; }
        set pathname(value) { this.#set("pathname", value); }
        get search() { return this.#parts.search; }
        set search(value) { this.#set("search", value); }
        get hash() { return this.#parts.hash; }
        set hash(value) { this.#set("hash", value); }
        get searchParams() { return this.#searchParams; }
        toString() { return this.#parts.href; }
        toJSON() { return this.#parts.href; }
        get [Symbol.toStringTag]() { return "URL"; }
    }
    target.URL = URL;
    target.URLSearchParams = URLSearchParams;
}
