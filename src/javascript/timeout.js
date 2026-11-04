// @ts-check
// Public timer semantics over the C++ scheduler-backed implementation.
import { timeoutImpl } from "amj:timeout_impl";

/**
 * @template {Array<unknown>} Arguments
 * @param {(...args: Arguments) => void} callback
 * @param {number} [delay]
 * @param {Arguments} args
 * @returns {number}
 */
export function setTimeout(callback, delay = 0, ...args) {
  if (typeof callback !== "function") {
    throw new TypeError("setTimeout requires a function callback");
  }
  if (args.length > 256) {
    throw new RangeError("Timer argument limit exceeded");
  }
  let milliseconds = +delay;
  if (Number.isNaN(milliseconds) || milliseconds < 0) {
    milliseconds = 0;
  } else if (milliseconds > 2147483647) {
    milliseconds = 2147483647;
  }
  milliseconds = Math.trunc(milliseconds);
  return timeoutImpl.setTimeout(() => callback(...args), milliseconds);
}

/**
 * @param {number} [id]
 * @returns {void}
 */
export function clearTimeout(id) {
  if (id === undefined) {
    return;
  }
  const timerID = +id;
  if (!Number.isSafeInteger(timerID) || timerID < 1) {
    return;
  }
  timeoutImpl.clearTimeout(timerID);
}
