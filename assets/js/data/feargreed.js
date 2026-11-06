/* Daily Fear & Greed index (alternative.me), through the local data program. */
import { api } from "./http.js";

export const fetchFearGreed = () => api("/api/sentiment/feargreed");
