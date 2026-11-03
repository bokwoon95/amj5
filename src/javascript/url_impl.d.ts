// JavaScript representation of the boundary types and operations in url_impl.hpp.
declare module "amj:url_impl" {
  export type URLComponent =
    "href" | "protocol" | "username" | "password" | "host" | "hostname" | "port" | "pathname" | "search" | "hash";

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

  export function urlParse(input: string, base?: string | undefined): URLParts | null;
  export function urlWithComponent(href: string, component: URLComponent, value: string): URLParts | null;
  export function queryStringParse(queryString: string): Array<[string, string]>;
  export function queryStringify(entries: Array<[string, string]>): string;
}
