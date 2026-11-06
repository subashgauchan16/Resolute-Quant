/* The latest top of book sent by the data program, which holds the full book. No DOM. */
export const B = { product: null, bids: [], asks: [], depth: 0, ready: false };

export function setBook(m) {
  B.product = m.id;
  B.bids = m.bids;
  B.asks = m.asks;
  B.depth = m.depth;
  B.ready = true;
}

export function bookReset() {
  B.ready = false;
  B.bids = [];
  B.asks = [];
  B.depth = 0;
}

/* Top levels of one side, each with its cumulative size. */
export function bview(side, levels) {
  let c = 0;
  return (side === "buy" ? B.bids : B.asks).slice(0, levels).map(([p, s]) => {
    c += s;
    return { p, s, c };
  });
}
