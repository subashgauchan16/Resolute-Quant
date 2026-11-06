/* Namespaced JSON access to localStorage. Never throws, so private windows still work. */
const PREFIX = "subash.";

export const store = {
  get(key, fallback) {
    try {
      const value = localStorage.getItem(PREFIX + key);
      return value ? JSON.parse(value) : fallback;
    } catch {
      return fallback;
    }
  },
  set(key, value) {
    try {
      localStorage.setItem(PREFIX + key, JSON.stringify(value));
    } catch {
      /* storage full or blocked: the session still works, it just will not persist */
    }
  },
};
