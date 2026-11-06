/* Calls to the local data program, the page's only data source. Errors carry `status`. */
export async function api(path) {
  const response = await fetch(path);
  const body = await response.json().catch(() => ({}));
  if (!response.ok) throw Object.assign(new Error(body.error || response.status), { status: response.status });
  return body;
}
