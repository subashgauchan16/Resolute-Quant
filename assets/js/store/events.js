/* Synchronous event bus. It lets the data layer report news without knowing about the UI.
   Events: trade, price, feed-restored, markets-link, select. */
const listeners = new Map();

export function on(name, listener) {
  if (!listeners.has(name)) listeners.set(name, []);
  listeners.get(name).push(listener);
}

export function emit(name, ...args) {
  for (const listener of listeners.get(name) || []) listener(...args);
}
