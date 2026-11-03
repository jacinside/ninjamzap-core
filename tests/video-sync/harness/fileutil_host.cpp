// Host build of the adapter's one platform hook (iOS: fileutil.mm, Android: fileutil_android.cpp):
// where it reads its properties file. A temp dir keeps the user's home untouched.
#include "fileutil.h"

namespace AbNinjam {
namespace Common {

path getHomePath() { return temp_directory_path(); }

} // namespace Common
} // namespace AbNinjam
