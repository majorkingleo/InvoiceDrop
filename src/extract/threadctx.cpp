#include "extract/threadctx.h"

#include <mupdf/fitz.h>

namespace InvoiceDrop::Extract::ThreadCtx {
namespace {

/// Creates the context eagerly and drops it from the thread_local destructor,
/// so a thread that only touches MuPDF once still cleans up.
class Holder
{
public:
    Holder() { create(); }

    ~Holder() { drop(); }

    fz_context *get() const { return m_context; }

    void drop()
    {
        if (m_context) {
            fz_drop_context(m_context);
            m_context = nullptr;
        }
    }

private:
    void create()
    {
        // MuPDF throws C++ exceptions when compiled as C++, so every entry
        // point is guarded.
        try {
            m_context = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
            if (m_context)
                fz_register_document_handlers(m_context);
        } catch (...) {
            m_context = nullptr;
        }
    }

    fz_context *m_context = nullptr;
};

Holder &holder()
{
    thread_local Holder instance;
    return instance;
}

} // namespace

fz_context *mupdf()
{
    return holder().get();
}

void releaseMupdf()
{
    holder().drop();
}

} // namespace InvoiceDrop::Extract::ThreadCtx
