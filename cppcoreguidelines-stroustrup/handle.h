/**
 * RAII ownership handles for the reference counted GStreamer and GLib C types.
 *
 * GStreamer objects carry an intrusive reference count, so they are owned through
 * boost::intrusive_ptr, which borrows that count instead of allocating one of its own.
 * Types that are not reference counted (GError) are owned by a plain unique_ptr.
 *
 * See https://gstreamer.freedesktop.org/documentation/tutorials/basic/understanding-gstobject.html
 * for more information on GStreamer reference counting.
 */

#ifndef GSTU_GST_HANDLE_H
#define GSTU_GST_HANDLE_H

#include <glib.h>
#include <gst/gst.h>

#include <boost/intrusive_ptr.hpp>

#include <memory>
#include <type_traits>

// gcc-13 is missing std::out_ptr, even though it claims support for C++23.
// Use ztd::out_ptr as a fallback if std::out_ptr is not available.
#if __cpp_lib_out_ptr < 202106l
# include <ztd/out_ptr.hpp>
#endif

namespace gstu {

// GObject inheritance is invisible to C++, so each type's family is declared here, not deduced.
// List unqualified types only; query membership through is_gst_object and is_gst_mini_object.

template<typename T>
inline constexpr bool enable_gst_object = false;

template<> inline constexpr bool enable_gst_object<GstObject> = true;
template<> inline constexpr bool enable_gst_object<GstElement> = true;
template<> inline constexpr bool enable_gst_object<GstBin> = true;
template<> inline constexpr bool enable_gst_object<GstPipeline> = true;
template<> inline constexpr bool enable_gst_object<GstElementFactory> = true;
template<> inline constexpr bool enable_gst_object<GstPad> = true;
template<> inline constexpr bool enable_gst_object<GstPadTemplate> = true;
template<> inline constexpr bool enable_gst_object<GstBus> = true;
template<> inline constexpr bool enable_gst_object<GstClock> = true;

template<typename T>
inline constexpr bool enable_gst_mini_object = false;

template<> inline constexpr bool enable_gst_mini_object<GstCaps> = true;
template<> inline constexpr bool enable_gst_mini_object<GstBuffer> = true;
template<> inline constexpr bool enable_gst_mini_object<GstBufferList> = true;
template<> inline constexpr bool enable_gst_mini_object<GstEvent> = true;
template<> inline constexpr bool enable_gst_mini_object<GstMessage> = true;
template<> inline constexpr bool enable_gst_mini_object<GstQuery> = true;
template<> inline constexpr bool enable_gst_mini_object<GstSample> = true;
template<> inline constexpr bool enable_gst_mini_object<GstContext> = true;

template<typename T>
inline constexpr bool is_gst_object = enable_gst_object<std::remove_cv_t<T>>;

template<typename T>
inline constexpr bool is_gst_mini_object = enable_gst_mini_object<std::remove_cv_t<T>>;

namespace detail {

	// Every mini object starts with its GstMiniObject; GST_MINI_OBJECT_CAST relies on the same.
	template<typename T>
		requires is_gst_mini_object<T>
	GstMiniObject* as_mini_object(T* object) noexcept
	{
		// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
		return reinterpret_cast<GstMiniObject*>(object);
	}

} // namespace detail

} // namespace gstu

/*
 * boost::intrusive_ptr reaches its reference counting hooks through ADL. The GStreamer types
 * are C structs declared at global scope, so the global namespace is the only associated
 * namespace and the hooks have to be defined here rather than in gstu.
 *
 * boost::intrusive_ptr never passes a null pointer to them, which is what the GStreamer ref
 * and unref functions require. Counting mutates the object, so cv-qualified pointees are
 * rejected by the constraints rather than cast.
 */

// GstObject derived types, reference counted through the GObject machinery.
template<typename T>
	requires gstu::is_gst_object<T> && std::is_same_v<T, std::remove_cv_t<T>>
void intrusive_ptr_add_ref(T* object) noexcept
{
	gst_object_ref(object);
}

template<typename T>
	requires gstu::is_gst_object<T> && std::is_same_v<T, std::remove_cv_t<T>>
void intrusive_ptr_release(T* object) noexcept
{
	gst_object_unref(object);
}

// GstMiniObject derived types, which have their own lighter reference counting.
template<typename T>
	requires gstu::is_gst_mini_object<T> && std::is_same_v<T, std::remove_cv_t<T>>
void intrusive_ptr_add_ref(T* object) noexcept
{
	gst_mini_object_ref(gstu::detail::as_mini_object(object));
}

template<typename T>
	requires gstu::is_gst_mini_object<T> && std::is_same_v<T, std::remove_cv_t<T>>
void intrusive_ptr_release(T* object) noexcept
{
	gst_mini_object_unref(gstu::detail::as_mini_object(object));
}

namespace gstu {

// ----- [ Shared ownership handles ] -----

template<typename T>
	requires gstu::is_gst_mini_object<T> || gstu::is_gst_object<T>
using Handle = boost::intrusive_ptr<T>;

using Object_ptr = Handle<GstObject>;
using Element_ptr = Handle<GstElement>;
using Bin_ptr = Handle<GstBin>;
using Pipeline_ptr = Handle<GstPipeline>;
using Element_factory_ptr = Handle<GstElementFactory>;
using Pad_ptr = Handle<GstPad>;
using Pad_template_ptr = Handle<GstPadTemplate>;
using Bus_ptr = Handle<GstBus>;
using Clock_ptr = Handle<GstClock>;
using Caps_ptr = Handle<GstCaps>;
using Buffer_ptr = Handle<GstBuffer>;
using Buffer_list_ptr = Handle<GstBufferList>;
using Event_ptr = Handle<GstEvent>;
using Message_ptr = Handle<GstMessage>;
using Query_ptr = Handle<GstQuery>;
using Sample_ptr = Handle<GstSample>;
using Context_ptr = Handle<GstContext>;

/*
 * boost::intrusive_ptr adds a reference by default, so its constructor implements the
 * "transfer none" convention. These factories name the convention of the wrapped call
 * instead of leaving it to a bare bool at the call site.
 */

// "transfer full": the caller already owns a reference, take it over.
template<typename T>
auto adopt(T* object) noexcept
{
	return Handle<T>{object, false};
}

// "transfer none": the callee keeps its reference, take one of our own.
template<typename T>
auto share(T* object) noexcept
{
	return Handle<T>{object};
}

// "transfer floating": GObject constructors hand out a floating reference, sink it first.
template<typename T>
	requires is_gst_object<T> && std::is_same_v<T, std::remove_cv_t<T>>
auto adopt_floating(T* object) noexcept
{
	return object ? gstu::adopt(static_cast<T*>(gst_object_ref_sink(object))) : nullptr;
}

// ----- [ Unique ownership handles ] -----

#if __cpp_lib_out_ptr >= 202106l
/**
 * Provides a resource-safe way to manage an dynamically allocated output pointer parameter
 * in a C API. Such output pointer parameters are common in GStreamer and GLib functions,
 * especially for reporting errors via GError.
 *
 * Example usage:
 *
 *   Error_ptr error; // Aka std::unique_ptr<GError, Error_deleter>
 *   gst_parse_launch(description, gstu::out_ptr(error));
 *
 * In the above example, `gstu::out_ptr(error)` ensures resource safety for the `GError` parameter.
 */
using std::out_ptr;
using std::inout_ptr;

#else
/*
 * Unfortunately, gcc-13 does not support std::out_ptr and std::inout_ptr even though it claims to
 * support C++23, so we fall back to using the ztd out_ptr library. The ztd out_ptr library is the
 * original reference implementation of the standard proposal for out_ptr.
 */
using ztd::out_ptr::out_ptr;
using ztd::out_ptr::inout_ptr;

#endif // __cpp_lib_out_ptr >= 202106l

struct Error_deleter {
	void operator()(GError* error) const noexcept { g_error_free(error); }
};

struct Chars_deleter {
	void operator()(char* text) const noexcept { g_free(text); }
};

// GError is not reference counted, so unique ownership is all it needs.
using Error_ptr = std::unique_ptr<GError, Error_deleter>;

// Owns a string that GLib allocated, as returned by gst_error_get_message(), the debug string
// of gst_message_parse_error(), gst_caps_to_string(), and friends.
using Chars_ptr = std::unique_ptr<char, Chars_deleter>;

} // namespace gstu

#endif // GSTU_GST_HANDLE_H
