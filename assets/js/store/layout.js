/* Saved dashboard layout. The Phase 2 panel controls read and write this. */
import { store } from "./localStorage.js";

const KEY = "layout";
const VERSION = 1;

const isStringList = (value) => Array.isArray(value) && value.every((item) => typeof item === "string");

function isSizes(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    && Object.values(value).every((size) => Number.isFinite(size) && size > 0);
}

export const defaultLayout = () => ({ version: VERSION, hidden: [], order: [], sizes: {} });

/* A saved layout from another version, or a damaged one, falls back to the default. */
export function loadLayout() {
  const saved = store.get(KEY, null);
  const usable = saved && saved.version === VERSION
    && isStringList(saved.hidden) && isStringList(saved.order) && isSizes(saved.sizes);
  return usable ? saved : defaultLayout();
}

export function saveLayout(layout) {
  store.set(KEY, { ...layout, version: VERSION });
}
