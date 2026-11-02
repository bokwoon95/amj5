// Installed directly by installTimeout() in timeout.cpp; no JS facade.
/** Schedule a function once. Delay is clamped to [0, 2147483647] milliseconds. */
declare function setTimeout<Arguments extends unknown[]>(
    callback: (...args: Arguments) => void,
    delay?: number,
    ...args: Arguments
): number;

/** Cancel a timer. Missing or unknown IDs have no effect. */
declare function clearTimeout(id?: number): void;
