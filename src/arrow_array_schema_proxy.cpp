// Copyright 2024 Man Group Operations Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "sparrow/arrow_array_schema_proxy.hpp"

#include "sparrow/arrow_interface/arrow_array.hpp"
#include "sparrow/arrow_interface/arrow_array_schema_info_utils.hpp"
#include "sparrow/arrow_interface/arrow_flag_utils.hpp"
#include "sparrow/arrow_interface/arrow_schema.hpp"
#include "sparrow/arrow_interface/arrow_schema/private_data.hpp"
#include "sparrow/buffer/dynamic_bitset/dynamic_bitset_view.hpp"
#include "sparrow/utils/contracts.hpp"


namespace sparrow
{
    arrow_proxy arrow_proxy::view()
    {
        return arrow_proxy(&array(), &schema());
    }

    void arrow_proxy::update_buffers()
    {
        m_buffers = get_arrow_array_buffers(array(), schema());
    }

    void arrow_proxy::update_children()
    {
        m_children.clear();
        m_children.reserve(n_children());
        for (size_t i = 0; i < n_children(); ++i)
        {
            m_children.emplace_back(array().children[i], schema().children[i]);
        }
    }

    void arrow_proxy::update_dictionary()
    {
        if (array().dictionary == nullptr || schema().dictionary == nullptr)
        {
            m_dictionary = nullptr;
        }
        else
        {
            m_dictionary = std::make_unique<arrow_proxy>(array().dictionary, schema().dictionary);
        }
    }

    void arrow_proxy::reset()
    {
        m_buffers.clear();
        m_children.clear();
        m_dictionary.reset();
    }

    bool arrow_proxy::array_created_with_sparrow() const
    {
        return array().release == &sparrow::release_arrow_array;
    }

    bool arrow_proxy::schema_created_with_sparrow() const
    {
        return schema().release == &sparrow::release_arrow_schema;
    }

    void arrow_proxy::validate_array_and_schema() const
    {
        SPARROW_ASSERT_TRUE(is_proxy_valid());
        SPARROW_ASSERT_TRUE(array().n_children == schema().n_children);
        SPARROW_ASSERT_TRUE((array().dictionary == nullptr) == (schema().dictionary == nullptr));

        const enum data_type data_type = format_to_data_type(schema().format);
        if (!validate_format_with_arrow_array(data_type, array()))
        {
            throw arrow_proxy_exception("Invalid ArrowArray format");
        }
    }

    template <typename AA, typename AS>
        requires std::same_as<std::remove_pointer_t<std::remove_cvref_t<AA>>, ArrowArray>
                     && std::same_as<std::remove_pointer_t<std::remove_cvref_t<AS>>, ArrowSchema>
    arrow_proxy::arrow_proxy(AA&& array, AS&& schema, impl_tag)
        : m_array(std::forward<AA>(array))
        , m_schema(std::forward<AS>(schema))
    {
        if constexpr (std::is_rvalue_reference_v<AA&&>)
        {
            array = {};
        }
        else if constexpr (std::is_pointer_v<std::remove_cvref_t<AA>>)
        {
            SPARROW_ASSERT_TRUE(array != nullptr);
        }

        if constexpr (std::is_rvalue_reference_v<AS&&>)
        {
            schema = {};
        }
        else if constexpr (std::is_pointer_v<std::remove_cvref_t<AS>>)
        {
            SPARROW_ASSERT_TRUE(schema != nullptr);
        }

        validate_array_and_schema();
        update_buffers();
        update_children();
        update_dictionary();
    }

    arrow_proxy::arrow_proxy(ArrowArray&& array, ArrowSchema&& schema)
        : arrow_proxy(std::move(array), std::move(schema), impl_tag{})
    {
    }

    arrow_proxy::arrow_proxy(ArrowArray&& array, ArrowSchema* schema)
        : arrow_proxy(std::move(array), schema, impl_tag{})
    {
    }

    arrow_proxy::arrow_proxy(ArrowArray* array, ArrowSchema* schema)
        : arrow_proxy(array, schema, impl_tag{})
    {
    }

    arrow_proxy::arrow_proxy(const arrow_proxy& other)
        : m_array(copy_array(other.array(), other.schema()))
        , m_schema(copy_schema(other.schema()))
    {
        validate_array_and_schema();
        update_buffers();
        update_children();
        update_dictionary();
    }

    arrow_proxy& arrow_proxy::operator=(const arrow_proxy& other)
    {
        if (this == &other)
        {
            return *this;
        }
        arrow_proxy tmp(other);
        swap(tmp);
        return *this;
    }

    arrow_proxy::arrow_proxy(arrow_proxy&& other)
        : m_array(std::move(other.m_array))
        , m_schema(std::move(other.m_schema))
    {
        other.m_array = {};
        other.m_schema = {};
        other.m_buffers.clear();
        update_buffers();
        update_children();
        update_dictionary();
    }

    arrow_proxy& arrow_proxy::operator=(arrow_proxy&& rhs)
    {
        swap(rhs);
        return *this;
    }

    arrow_proxy::~arrow_proxy()
    {
        if (m_array.index() == 1)  // We own the array
        {
            if (array().release != nullptr)
            {
                try
                {
                    array().release(&array());
                }
                catch (...)
                {
#if defined(SPARROW_CONTRACTS_DEFAULT_ABORT_ON_FAILURE)
                    SPARROW_CONTRACTS_DEFAULT_ON_FAILURE(
                        "~arrow_proxy",
                        "Exception thrown during array.release in arrow_proxy destructor."
                    );
#endif
                }
            }
        }

        if (m_schema.index() == 1)  // We own the schema
        {
            if (schema().release != nullptr)
            {
                try
                {
                    schema().release(&schema());
                }
                catch (...)
                {
#if defined(SPARROW_CONTRACTS_DEFAULT_ABORT_ON_FAILURE)
                    SPARROW_CONTRACTS_DEFAULT_ON_FAILURE(
                        "~arrow_proxy",
                        "Exception thrown during schema.release in arrow_proxy destructor."
                    );
#endif
                }
            }
        }
    }

    [[nodiscard]] const std::string_view arrow_proxy::format() const
    {
        return schema().format;
    }

    void arrow_proxy::set_format(const std::string_view format)
    {
        if (!is_created_with_sparrow())
        {
            throw arrow_proxy_exception("Cannot set format on non-sparrow created ArrowArray");
        }
#if defined(__GNUC__) && !defined(__clang__)  // Bypass the bug:
                                              // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=105651
#    if __GNUC__ == 12
#        pragma GCC diagnostic push
#        pragma GCC diagnostic ignored "-Wrestrict"
#    endif
#endif
        get_schema_private_data()->format() = format;
#if defined(__GNUC__) && !defined(__clang__)
#    if __GNUC__ == 12
#        pragma GCC diagnostic pop
#    endif
#endif
        schema().format = get_schema_private_data()->format_ptr();
    }

    [[nodiscard]] enum data_type arrow_proxy::data_type() const
    {
        return format_to_data_type(schema().format);
    }

    void arrow_proxy::set_data_type(enum data_type data_type)
    {
        if (!schema_created_with_sparrow())
        {
            throw arrow_proxy_exception("Cannot set data_type on non-sparrow created ArrowArray");
        }
        set_format(data_type_to_format(data_type));
        schema().format = get_schema_private_data()->format_ptr();
    }

    [[nodiscard]] std::optional<const std::string_view> arrow_proxy::name() const
    {
        if (schema().name == nullptr)
        {
            return std::nullopt;
        }
        return std::string_view(schema().name);
    }

    void arrow_proxy::set_name(std::optional<const std::string_view> name)
    {
        if (!schema_created_with_sparrow())
        {
            throw arrow_proxy_exception("Cannot set name on non-sparrow created ArrowArray");
        }
        get_schema_private_data()->name() = name;
        schema().name = get_schema_private_data()->name_ptr();
    }

    [[nodiscard]] std::optional<const std::string_view> arrow_proxy::metadata() const
    {
        if (schema().metadata == nullptr)
        {
            return std::nullopt;
        }
        return std::string_view(schema().metadata);
    }

    void arrow_proxy::set_metadata(std::optional<const std::string_view> metadata)
    {
        if (!schema_created_with_sparrow())
        {
            throw arrow_proxy_exception("Cannot set metadata on non-sparrow created ArrowArray");
        }
        get_schema_private_data()->metadata() = metadata;
        schema().metadata = get_schema_private_data()->metadata_ptr();
    }

    [[nodiscard]] std::vector<ArrowFlag> arrow_proxy::flags() const
    {
        return to_vector_of_ArrowFlags(schema().flags);
    }

    void arrow_proxy::set_flags(const std::vector<ArrowFlag>& flags)
    {
        if (!schema_created_with_sparrow())
        {
            throw arrow_proxy_exception("Cannot set flags on non-sparrow created ArrowArray");
        }
        schema().flags = to_ArrowFlag_value(flags);
    }

    [[nodiscard]] size_t arrow_proxy::length() const
    {
        SPARROW_ASSERT_TRUE(array().length >= 0);
        SPARROW_ASSERT_TRUE(std::cmp_less(array().length, std::numeric_limits<size_t>::max()));
        return static_cast<size_t>(array().length);
    }

    void arrow_proxy::set_length(size_t length)
    {
        SPARROW_ASSERT_TRUE(std::cmp_less(length, std::numeric_limits<int64_t>::max()));
        if (!array_created_with_sparrow())
        {
            throw arrow_proxy_exception("Cannot set length on non-sparrow created ArrowArray");
        }
        array().length = static_cast<int64_t>(length);
    }

    [[nodiscard]] int64_t arrow_proxy::null_count() const
    {
        return array().null_count;
    }

    void arrow_proxy::set_null_count(int64_t null_count)
    {
        if (!is_created_with_sparrow())
        {
            throw arrow_proxy_exception("Cannot set null_count on non-sparrow created ArrowArray");
        }
        array().null_count = null_count;
    }

    [[nodiscard]] size_t arrow_proxy::offset() const
    {
        return static_cast<size_t>(array().offset);
    }

    void arrow_proxy::set_offset(size_t offset)
    {
        SPARROW_ASSERT_TRUE(std::cmp_less(offset, std::numeric_limits<int64_t>::max()));
        if (!array_created_with_sparrow())
        {
            throw arrow_proxy_exception("Cannot set offset on non-sparrow created ArrowArray");
        }
        array().offset = static_cast<int64_t>(offset);
    }

    [[nodiscard]] size_t arrow_proxy::n_buffers() const
    {
        return static_cast<size_t>(array().n_buffers);
    }

    void arrow_proxy::set_n_buffers(size_t n_buffers)
    {
        SPARROW_ASSERT_TRUE(std::cmp_less(n_buffers, std::numeric_limits<int64_t>::max()));
        if (!array_created_with_sparrow())
        {
            throw arrow_proxy_exception("Cannot set n_buffers on non-sparrow created ArrowArray");
        }
        array().n_buffers = static_cast<int64_t>(n_buffers);
        arrow_array_private_data* private_data = get_array_private_data();
        private_data->resize_buffers(n_buffers);
        array().buffers = private_data->buffers_ptrs<void>();
        array().n_buffers = static_cast<int64_t>(n_buffers);
    }

    [[nodiscard]] size_t arrow_proxy::n_children() const
    {
        return static_cast<size_t>(array().n_children);
    }

    void arrow_proxy::pop_children(size_t n)
    {
        if (!is_created_with_sparrow())
        {
            throw arrow_proxy_exception("Cannot set n_buffers on non-sparrow created ArrowArray or ArrowSchema");
        }

        if (n > n_children())
        {
            throw arrow_proxy_exception("Cannot pop more children than the current number of children");
        }

        resize_children(n_children() - n);
        update_children();
    }

    void arrow_proxy::resize_children(size_t children_count)
    {
        SPARROW_ASSERT_TRUE(std::cmp_less(children_count, std::numeric_limits<int64_t>::max()));
        if (!is_created_with_sparrow())
        {
            throw arrow_proxy_exception("Cannot set n_children on non-sparrow created ArrowArray or ArrowSchema"
            );
        }

        // Check that the release callback is valid for all children
        for (size_t i = 0; i < n_children(); ++i)
        {
            SPARROW_ASSERT_TRUE(schema().children[i] != nullptr);
            SPARROW_ASSERT_TRUE(schema().children[i]->release != nullptr);
            SPARROW_ASSERT_TRUE(array().children[i] != nullptr);
            SPARROW_ASSERT_TRUE(array().children[i]->release != nullptr);
        }

        // Release the remaining children if the new size is smaller than the current size
        for (size_t i = children_count; i < static_cast<size_t>(array().n_children); ++i)
        {
            schema().children[i]->release(schema().children[i]);
            array().children[i]->release(array().children[i]);
        }

        auto arrow_array_children = new ArrowArray*[children_count];
        auto arrow_schemas_children = new ArrowSchema*[children_count];
        for (size_t i = 0; i < std::min(children_count, static_cast<size_t>(array().n_children)); ++i)
        {
            arrow_array_children[i] = array().children[i];
            arrow_schemas_children[i] = schema().children[i];
        }

        delete[] array().children;
        array().children = arrow_array_children;
        array().n_children = static_cast<int64_t>(children_count);
        delete[] schema().children;
        schema().children = arrow_schemas_children;
        schema().n_children = static_cast<int64_t>(children_count);
    }

    arrow_schema_private_data* arrow_proxy::get_schema_private_data()
    {
        SPARROW_ASSERT_TRUE(schema_created_with_sparrow());
        return static_cast<arrow_schema_private_data*>(schema().private_data);
    }

    arrow_array_private_data* arrow_proxy::get_array_private_data()
    {
        SPARROW_ASSERT_TRUE(array_created_with_sparrow());
        return static_cast<arrow_array_private_data*>(array().private_data);
    }

    [[nodiscard]] const std::vector<sparrow::buffer_view<uint8_t>>& arrow_proxy::buffers() const
    {
        return m_buffers;
    }

    [[nodiscard]] std::vector<sparrow::buffer_view<uint8_t>>& arrow_proxy::buffers()
    {
        return m_buffers;
    }

    void arrow_proxy::set_buffer(size_t index, const buffer_view<uint8_t>& buffer)
    {
        SPARROW_ASSERT_TRUE(std::cmp_less(index, n_buffers()));
        if (!array_created_with_sparrow())
        {
            throw arrow_proxy_exception("Cannot set buffer on non-sparrow created ArrowArray");
        }
        auto array_private_data = get_array_private_data();
        array_private_data->set_buffer(index, buffer);
        array().buffers = array_private_data->buffers_ptrs<void>();
        update_null_count();
        update_buffers();
    }

    void arrow_proxy::set_buffer(size_t index, buffer<uint8_t>&& buffer)
    {
        SPARROW_ASSERT_TRUE(std::cmp_less(index, n_buffers()));
        if (!array_created_with_sparrow())
        {
            throw arrow_proxy_exception("Cannot set buffer on non-sparrow created ArrowArray");
        }
        auto array_private_data = get_array_private_data();
        array_private_data->set_buffer(index, std::move(buffer));
        array().buffers = array_private_data->buffers_ptrs<void>();
        update_null_count();
        update_buffers();
    }

    [[nodiscard]] const std::vector<arrow_proxy>& arrow_proxy::children() const
    {
        return m_children;
    }

    [[nodiscard]] std::vector<arrow_proxy>& arrow_proxy::children()
    {
        return m_children;
    }

    void arrow_proxy::set_child(size_t index, ArrowArray* child_array, ArrowSchema* child_schema)
    {
        SPARROW_ASSERT_TRUE(std::cmp_less(index, n_children()));
        if (!is_created_with_sparrow())
        {
            throw arrow_proxy_exception("Cannot set child on non-sparrow created ArrowArray or ArrowSchema");
        }
        array().children[index] = child_array;
        schema().children[index] = child_schema;
        update_children();
    }

    [[nodiscard]] const std::unique_ptr<arrow_proxy>& arrow_proxy::dictionary() const
    {
        return m_dictionary;
    }

    [[nodiscard]] std::unique_ptr<arrow_proxy>& arrow_proxy::dictionary()
    {
        return m_dictionary;
    }

    void arrow_proxy::set_dictionary(ArrowArray* array_dictionary, ArrowSchema* schema_dictionary)
    {
        if (!is_created_with_sparrow())
        {
            throw arrow_proxy_exception("Cannot set dictionary on non-sparrow created ArrowArray or ArrowSchema"
            );
        }

        if (array().dictionary != nullptr)
        {
            array().dictionary->release(array().dictionary);
        }
        if (schema().dictionary != nullptr)
        {
            schema().dictionary->release(schema().dictionary);
        }

        array().dictionary = array_dictionary;
        schema().dictionary = schema_dictionary;
        update_dictionary();
    }

    [[nodiscard]] bool arrow_proxy::is_created_with_sparrow() const
    {
        return array_created_with_sparrow() && schema_created_with_sparrow();
    }

    [[nodiscard]] void* arrow_proxy::private_data() const
    {
        return array().private_data;
    }

    template <typename T>
    [[nodiscard]] T& get_value_reference_of_variant(auto& var)
    {
        return var.index() == 0 ? *std::get<0>(var) : std::get<1>(var);
    }

    [[nodiscard]] bool arrow_proxy::owns_array() const
    {
        return std::holds_alternative<ArrowArray>(m_array);
    }

    [[nodiscard]] bool arrow_proxy::owns_schema() const
    {
        return std::holds_alternative<ArrowSchema>(m_schema);
    }

    [[nodiscard]] const ArrowArray& arrow_proxy::array() const
    {
        return get_value_reference_of_variant<const ArrowArray>(m_array);
    }

    [[nodiscard]] const ArrowSchema& arrow_proxy::schema() const
    {
        return get_value_reference_of_variant<const ArrowSchema>(m_schema);
    }

    [[nodiscard]] ArrowArray& arrow_proxy::array()
    {
        return get_value_reference_of_variant<ArrowArray>(m_array);
    }

    [[nodiscard]] ArrowSchema& arrow_proxy::schema()
    {
        return get_value_reference_of_variant<ArrowSchema>(m_schema);
    }

    [[nodiscard]] ArrowArray arrow_proxy::extract_array()
    {
        if (std::holds_alternative<ArrowArray*>(m_array))
        {
            throw std::runtime_error("cannot extract an ArrowArray not owned by the structure");
        }

        ArrowArray res = std::get<ArrowArray>(std::move(m_array));
        m_array = ArrowArray{};
        reset();
        return res;
    }

    [[nodiscard]] ArrowSchema arrow_proxy::extract_schema()
    {
        if (std::holds_alternative<ArrowSchema*>(m_schema))
        {
            throw std::runtime_error("cannot extract an ArrowSchema not owned by the structure");
        }

        ArrowSchema res = std::get<ArrowSchema>(std::move(m_schema));
        m_schema = ArrowSchema{};
        reset();
        return res;
    }

    void arrow_proxy::update_null_count()
    {
        const auto buffer_types = get_buffer_types_from_data_type(data_type());
        const auto validity_it = std::ranges::find(buffer_types, buffer_type::VALIDITY);
        if (validity_it == buffer_types.end())
        {
            return;
        }
        const auto validity_index = std::distance(buffer_types.begin(), validity_it);
        auto& validity_buffer = buffers()[static_cast<size_t>(validity_index)];
        const dynamic_bitset_view<std::uint8_t> bitmap(validity_buffer.data(), validity_buffer.size());
        const auto null_count = bitmap.null_count();
        set_null_count(static_cast<int64_t>(null_count));
    }

    bool arrow_proxy::is_arrow_array_valid() const
    {
        return array().release != nullptr;
    }

    bool arrow_proxy::is_arrow_schema_valid() const
    {
        return schema().release != nullptr;
    }

    bool arrow_proxy::is_proxy_valid() const
    {
        return is_arrow_array_valid() && is_arrow_schema_valid();
    }

    void arrow_proxy::swap(arrow_proxy& other) noexcept
    {
        if (this == &other)
        {
            return;
        }
        std::swap(m_array, other.m_array);
        std::swap(m_schema, other.m_schema);
        std::swap(m_buffers, other.m_buffers);
        std::swap(m_children, other.m_children);
        std::swap(m_dictionary, other.m_dictionary);
    }
}