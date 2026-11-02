// JavaScript representation of the normalized operations in timeout_impl.hpp.
declare module "amj:timeout_impl" {
    export interface TimeoutImpl {
        /** callback receives no arguments; delay is an integer in [0, 2147483647]. */
        setTimeout(callback: () => void, delay: number): number;
        clearTimeout(timerID: number): void;
    }
    export const timeoutImpl: TimeoutImpl;
}
