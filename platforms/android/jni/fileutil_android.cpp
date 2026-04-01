#include "abNinjam/include/fileutil.h"
#include <cstdlib>

namespace AbNinjam {
namespace Common {

path getHomePath() {
    // On Android, use the app's internal storage path.
    // The actual path will be set via JNI from Kotlin (Context.getFilesDir()).
    // For now, fall back to a reasonable default.
    const char* home = getenv("HOME");
    if (home) return path(home);
    return path("/data/local/tmp");
}

} // namespace Common
} // namespace AbNinjam
