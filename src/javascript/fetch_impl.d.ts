// C++ contract from fetch_impl.hpp. Request handles are opaque to JavaScript.
declare module "amj:fetch" {
    export type HeaderPair = [string, string];
    export interface NativeRequest {
        readonly __nativeFetchRequest: unique symbol;
    }
    export interface NativeFailure {
        type: string;
        message: string;
    }
    export interface ResponseMetadata {
        status: number;
        headers: HeaderPair[];
    }
    export interface FetchImpl {
        start(url: string, method: string, headerPairs: HeaderPair[], bytes: Uint8Array,
            onHeaders: (failure: NativeFailure | null, metadata: ResponseMetadata | null) => void): NativeRequest;
        read(request: NativeRequest,
            onRead: (failure: NativeFailure | null, chunk: Uint8Array | null) => void): void;
        close(request: NativeRequest): void;
    }
    export const fetchImpl: FetchImpl;
}

declare var Headers: typeof import("./fetch.js").Headers;
type Headers = import("./fetch.js").Headers;
declare var fetch: typeof import("./fetch.js").fetch;
