// Loaded last, once every other script has defined its functions. A file
// rather than an inline <script>, because the Content-Security-Policy the
// server sends allows only the UI's own script files to run.
boot();
