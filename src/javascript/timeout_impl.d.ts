// C++ exports from TimeoutImpl::createModule(); no JS facade.
declare module "amj:timeout_impl" {
    /** Schedule a function once. Delay is clamped to [0, 2147483647] milliseconds. */
    export function setTimeout<Arguments extends unknown[]>(
        callback: (...args: Arguments) => void,
        delay?: number,
        ...args: Arguments
    ): number;

    /** Cancel a timer. Missing or unknown IDs have no effect. */
    export function clearTimeout(id?: number): void;
}
