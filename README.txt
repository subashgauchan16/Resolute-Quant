SUBASH TERMINAL

Start it
1. Unzip this folder somewhere you can keep it. Do not run it from inside the zip.
2. Double-click "Start Terminal.bat". The first run installs what it needs.
3. Your browser opens http://127.0.0.1:8765. Keep the black window open.
4. Close the black window to stop it.

Where the data comes from
Crypto: Coinbase Exchange, pushed live to the page.
Stocks, indices, FX and gold: Yahoo Finance through yfinance.
Yahoo's data is for personal use, can be delayed, and can stop working without notice.
The page shows the market state and any delay Yahoo lists for each symbol.

Commands
Press / then type HELP to see them all. Examples: AAPL, VOD.L, ETH, 1D, ALERT 77000.

If something stops working
Stocks show YAHOO PAUSED: Yahoo is limiting requests. The terminal slows down and retries.
Stocks show STOCKS OFF: the black window has been closed. Start it again.
Yahoo data breaks after a Yahoo change: delete the .venv folder and start again to get
the newest yfinance.

Nothing here connects to a broker or places orders. Only your own PC can reach the data
program, because it listens on 127.0.0.1.
