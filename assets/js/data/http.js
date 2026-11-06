/* Calls to the local data program, the page's only data source. Errors carry `status`. */
export async function apiText(path) {
  const response = await fetch(path);
  if (response.ok) return response.text();
  const body = await response.json().catch(() => ({}));
  throw Object.assign(new Error(body.error || response.status), { status: response.status });
}

export async function api(path) {
  const response = await fetch(path);
  const body = await response.json().catch(() => ({}));
  if (!response.ok) throw Object.assign(new Error(body.error || response.status), { status: response.status });
  return body;
}
