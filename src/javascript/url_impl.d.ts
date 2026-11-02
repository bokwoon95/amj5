// JavaScript representation of the boundary types and operations in url_impl.hpp.
declare module "amj:url_impl" {
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
        parseQuery(query: string): [string, string][];
        serializeQuery(pairs: [string, string][]): string;
    }
    export const urlImpl: URLImpl;
}
