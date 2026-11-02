// C++ contract from url_impl.hpp; no JavaScript implementation exists for this module.
declare module "amj:url" {
    export type QueryPair = [string, string];
    export type URLComponent = "href" | "protocol" | "username" | "password" | "host" | "hostname" | "port" | "pathname" | "search" | "hash";
    export interface URLParts {
        href: string;
        origin: string;
        protocol: string;
        username: string;
        password: string;
        host: string;
        hostname: string;
        port: string;
        pathname: string;
        search: string;
        hash: string;
    }
    export interface URLImpl {
        parse(input: string, baseOrNull: string | null): URLParts | null;
        set(href: string, component: URLComponent, value: string): URLParts | null;
        parseQuery(query: string): QueryPair[];
        serializeQuery(pairs: QueryPair[]): string;
    }
    export const urlImpl: URLImpl;
}

// The host installs these JS exports as globals for user scripts.
declare var URL: typeof import("./url.js").URL;
type URL = import("./url.js").URL;
declare var URLSearchParams: typeof import("./url.js").URLSearchParams;
type URLSearchParams = import("./url.js").URLSearchParams;
