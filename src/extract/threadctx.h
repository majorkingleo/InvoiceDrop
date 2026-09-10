#pragma once

struct fz_context;

namespace InvoiceDrop::Extract::ThreadCtx {

/// MuPDF context owned by the calling thread, created on first use.
///
/// MuPDF is not thread safe across contexts, so every thread gets its own.
/// The context is registered with the default document handlers and dropped
/// automatically when the thread ends.
fz_context *mupdf();

/// Drops the calling thread's MuPDF context explicitly. Optional: the holder
/// does this on thread exit anyway. Needed only when a context must be released
/// while the thread keeps running.
void releaseMupdf();

} // namespace InvoiceDrop::Extract::ThreadCtx
