/***
* ==++==
*
* Copyright (c) Microsoft Corporation. All rights reserved. 
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
* http://www.apache.org/licenses/LICENSE-2.0
* 
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* ==--==
* =+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
*
* oauth2_tests.cpp
*
* Test cases for oauth2.
*
* =-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
****/

#include "stdafx.h"

using namespace web;
using namespace web::http;
using namespace web::http::client;
using namespace web::http::details;
using namespace utility;
using namespace concurrency;

using namespace tests::functional::http::utilities;
extern utility::string_t _to_base64(const unsigned char *ptr, size_t size);

namespace tests { namespace functional { namespace http { namespace client {


static std::vector<unsigned char> to_body_data(utility::string_t str)
{
    const std::string utf8(conversions::to_utf8string(std::move(str)));
    return std::vector<unsigned char>(utf8.data(), utf8.data() + utf8.size());
}

SUITE(oauth2_tests)
{

struct oauth2_test_setup
{
    oauth2_test_setup() :
        m_uri(U("http://localhost:16743/")),
        m_oauth2_config(U("123ABC"), U("456DEF"), U("https://foo"), m_uri.to_string(), U("https://bar")),
        m_scoped(m_uri)
    {}

    web::http::uri m_uri;
    oauth2_config m_oauth2_config;
    test_http_server::scoped_server m_scoped;
};

TEST(oauth2_build_authorization_uri)
{
    oauth2_config config(U(""), U(""), U(""), U(""), U(""));
    config.set_state(U("xyzzy"));
    config.set_implicit_grant(false);

    // Empty authorization URI.
    {
        VERIFY_ARE_EQUAL(U("/?response_type=code&client_id=&redirect_uri=&state=xyzzy"),
                config.build_authorization_uri(false));
    }

    // Authorization URI with scope parameter.
    {
        config.set_scope(U("testing_123"));
        VERIFY_ARE_EQUAL(U("/?response_type=code&client_id=&redirect_uri=&state=xyzzy&scope=testing_123"),
                config.build_authorization_uri(false));
    }

    // Full authorization URI with scope.
    {
        config.set_client_key(U("4567abcd"));
        config.set_auth_endpoint(U("https://foo"));
        config.set_redirect_uri(U("http://localhost:8080"));
        VERIFY_ARE_EQUAL(U("https://foo/?response_type=code&client_id=4567abcd&redirect_uri=http://localhost:8080&state=xyzzy&scope=testing_123"),
                config.build_authorization_uri(false));
    }

    // Verify again with implicit grant.
    {
        config.set_implicit_grant(true);
        VERIFY_ARE_EQUAL(U("https://foo/?response_type=token&client_id=4567abcd&redirect_uri=http://localhost:8080&state=xyzzy&scope=testing_123"),
                config.build_authorization_uri(false));
    }

    // Verify that a new state() will be generated.
    {
        const uri auth_uri(config.build_authorization_uri(true));
        auto params = uri::split_query(auth_uri.query());
        VERIFY_ARE_NOT_EQUAL(params[U("state")], U("xyzzy"));
    }
}

TEST_FIXTURE(oauth2_test_setup, oauth2_token_from_code)
{
    VERIFY_IS_FALSE(m_oauth2_config.is_enabled());

    // Fetch using HTTP Basic authentication.
    {
        m_scoped.server()->next_request().then([](test_request *request)
        {
            VERIFY_ARE_EQUAL(request->m_method, methods::POST);

            utility::string_t content, charset;
            parse_content_type_and_charset(request->m_headers[header_names::content_type], content, charset);
            VERIFY_ARE_EQUAL(mime_types::application_x_www_form_urlencoded, content);

            VERIFY_ARE_EQUAL(U("Basic MTIzQUJDOjQ1NkRFRg=="), request->m_headers[header_names::authorization]);

            VERIFY_ARE_EQUAL(to_body_data(
                    U("grant_type=authorization_code&code=789GHI&redirect_uri=https%3A%2F%2Fbar")),
                    request->m_body);

            std::map<utility::string_t, utility::string_t> headers;
            headers[header_names::content_type] = mime_types::application_json;
            request->reply(status_codes::OK, U(""), headers, "{\"access_token\":\"xyzzy123\",\"token_type\":\"bearer\"}");
        });

        m_oauth2_config.token_from_code(U("789GHI")).wait();
        VERIFY_ARE_EQUAL(U("xyzzy123"), m_oauth2_config.token().access_token());
        VERIFY_IS_TRUE(m_oauth2_config.is_enabled());
    }

    // Fetch using client key & secret in request body (x-www-form-urlencoded).
    {
        m_scoped.server()->next_request().then([](test_request *request)
        {
            utility::string_t content;
            utility::string_t charset;
            parse_content_type_and_charset(request->m_headers[header_names::content_type], content, charset);
            VERIFY_ARE_EQUAL(mime_types::application_x_www_form_urlencoded, content);

            VERIFY_ARE_EQUAL(U(""), request->m_headers[header_names::authorization]);

            VERIFY_ARE_EQUAL(to_body_data(
                    U("grant_type=authorization_code&code=789GHI&redirect_uri=https%3A%2F%2Fbar&client_id=123ABC&client_secret=456DEF")),
                    request->m_body);

            std::map<utility::string_t, utility::string_t> headers;
            headers[header_names::content_type] = mime_types::application_json;
            request->reply(status_codes::OK, U(""), headers, "{\"access_token\":\"xyzzy123\",\"token_type\":\"bearer\"}");
        });

        m_oauth2_config.set_token(oauth2_token()); // Clear token.
        VERIFY_IS_FALSE(m_oauth2_config.is_enabled());

        m_oauth2_config.set_http_basic_auth(false);
        m_oauth2_config.token_from_code(U("789GHI")).wait();

        VERIFY_ARE_EQUAL(U("xyzzy123"), m_oauth2_config.token().access_token());
        VERIFY_IS_TRUE(m_oauth2_config.is_enabled());
    }
}

TEST_FIXTURE(oauth2_test_setup, oauth2_token_from_redirected_uri)
{
    // Authorization code grant.
    {
        m_scoped.server()->next_request().then([](test_request *request)
        {
            std::map<utility::string_t, utility::string_t> headers;
            headers[header_names::content_type] = mime_types::application_json;
            request->reply(status_codes::OK, U(""), headers, "{\"access_token\":\"foo\",\"token_type\":\"bearer\"}");
        });
    
        m_oauth2_config.set_implicit_grant(false);
        m_oauth2_config.set_state(U("xyzzy"));

        const web::http::uri redirected_uri(m_uri.to_string() + U("?code=sesame&state=xyzzy"));
        m_oauth2_config.token_from_redirected_uri(redirected_uri).wait();

        VERIFY_IS_TRUE(m_oauth2_config.token().is_valid());
        VERIFY_ARE_EQUAL(m_oauth2_config.token().access_token(), U("foo"));
    }

    // Implicit grant.
    {
        m_oauth2_config.set_implicit_grant(true);
        const web::http::uri redirected_uri(m_uri.to_string() + U("#access_token=abcd1234&state=xyzzy"));
        m_oauth2_config.token_from_redirected_uri(redirected_uri).wait();

        VERIFY_IS_TRUE(m_oauth2_config.token().is_valid());
        VERIFY_ARE_EQUAL(m_oauth2_config.token().access_token(), U("abcd1234"));
    }
}

TEST_FIXTURE(oauth2_test_setup, oauth2_token_from_refresh)
{
    oauth2_token token(U("accessing"));
    token.set_refresh_token(U("refreshing"));
    m_oauth2_config.set_token(token);
    VERIFY_IS_TRUE(m_oauth2_config.is_enabled());

    // Verify token refresh without scope.
    m_scoped.server()->next_request().then([](test_request *request)
    {
        VERIFY_ARE_EQUAL(request->m_method, methods::POST);

        utility::string_t content, charset;
        parse_content_type_and_charset(request->m_headers[header_names::content_type], content, charset);
        VERIFY_ARE_EQUAL(mime_types::application_x_www_form_urlencoded, content);

        VERIFY_ARE_EQUAL(U("Basic MTIzQUJDOjQ1NkRFRg=="), request->m_headers[header_names::authorization]);

        VERIFY_ARE_EQUAL(to_body_data(
                U("grant_type=refresh_token&refresh_token=refreshing")),
                request->m_body);

        std::map<utility::string_t, utility::string_t> headers;
        headers[header_names::content_type] = mime_types::application_json;
        request->reply(status_codes::OK, U(""), headers, "{\"access_token\":\"ABBA\",\"refresh_token\":\"BAZ\",\"token_type\":\"bearer\"}");
    });

    m_oauth2_config.token_from_refresh().wait();
    VERIFY_ARE_EQUAL(U("ABBA"), m_oauth2_config.token().access_token());
    VERIFY_ARE_EQUAL(U("BAZ"), m_oauth2_config.token().refresh_token());

    // Verify chaining refresh tokens and refresh with scope.
    m_scoped.server()->next_request().then([](test_request *request)
    {
        utility::string_t content, charset;
        parse_content_type_and_charset(request->m_headers[header_names::content_type], content, charset);

        VERIFY_ARE_EQUAL(to_body_data(
                U("grant_type=refresh_token&refresh_token=BAZ&scope=xyzzy")),
                request->m_body);

        std::map<utility::string_t, utility::string_t> headers;
        headers[header_names::content_type] = mime_types::application_json;
        request->reply(status_codes::OK, U(""), headers, "{\"access_token\":\"done\",\"token_type\":\"bearer\"}");
    });
    
    m_oauth2_config.set_scope(U("xyzzy"));
    m_oauth2_config.token_from_refresh().wait();
    VERIFY_ARE_EQUAL(U("done"), m_oauth2_config.token().access_token());
}

TEST_FIXTURE(oauth2_test_setup, oauth2_bearer_token)
{
    m_oauth2_config.set_token(oauth2_token(U("12345678")));
    http_client_config config;

    // Default, bearer token in "Authorization" header (bearer_auth() == true)
    {
        config.set_oauth2(m_oauth2_config);

        http_client client(m_uri, config);
        m_scoped.server()->next_request().then([](test_request *request)
        {
            VERIFY_ARE_EQUAL(U("Bearer 12345678"), request->m_headers[header_names::authorization]);
            VERIFY_ARE_EQUAL(U("/"), request->m_path);
            request->reply(status_codes::OK);
        });

        http_response response = client.request(methods::GET).get();
        VERIFY_ARE_EQUAL(status_codes::OK, response.status_code());
    }

    // Bearer token in query, default access token key (bearer_auth() == false)
    {
        m_oauth2_config.set_bearer_auth(false);
        config.set_oauth2(m_oauth2_config);

        http_client client(m_uri, config);
        m_scoped.server()->next_request().then([](test_request *request)
        {
            VERIFY_ARE_EQUAL(U(""), request->m_headers[header_names::authorization]);
            VERIFY_ARE_EQUAL(U("/?access_token=12345678"), request->m_path);
            request->reply(status_codes::OK);
        });

        http_response response = client.request(methods::GET).get();
        VERIFY_ARE_EQUAL(status_codes::OK, response.status_code());
    }

    // Bearer token in query, updated token, custom access token key (bearer_auth() == false)
    {
        m_oauth2_config.set_bearer_auth(false);
        m_oauth2_config.set_access_token_key(U("open"));
        m_oauth2_config.set_token(oauth2_token(U("Sesame")));
        config.set_oauth2(m_oauth2_config);

        http_client client(m_uri, config);
        m_scoped.server()->next_request().then([](test_request *request)
        {
            VERIFY_ARE_EQUAL(U(""), request->m_headers[header_names::authorization]);
            VERIFY_ARE_EQUAL(U("/?open=Sesame"), request->m_path);
            request->reply(status_codes::OK);
        });

        http_response response = client.request(methods::GET).get();
        VERIFY_ARE_EQUAL(status_codes::OK, response.status_code());
    }
}

TEST_FIXTURE(oauth2_test_setup, oauth2_token_parsing)
{
    VERIFY_IS_FALSE(m_oauth2_config.is_enabled());

    // Verify reply JSON 'access_token', 'refresh_token', 'expires_in' and 'scope'.
    {
        m_scoped.server()->next_request().then([](test_request *request)
        {
            std::map<utility::string_t, utility::string_t> headers;
            headers[header_names::content_type] = mime_types::application_json;
            request->reply(status_codes::OK, U(""), headers, "{\"access_token\":\"123\",\"refresh_token\":\"ABC\",\"token_type\":\"bearer\",\"expires_in\":12345678,\"scope\":\"baz\"}");
        });

        m_oauth2_config.token_from_code(U("")).wait();
        VERIFY_ARE_EQUAL(U("123"), m_oauth2_config.token().access_token());
        VERIFY_ARE_EQUAL(U("ABC"), m_oauth2_config.token().refresh_token());
        VERIFY_ARE_EQUAL(12345678, m_oauth2_config.token().expires_in());
        VERIFY_ARE_EQUAL(U("baz"), m_oauth2_config.token().scope());
        VERIFY_IS_TRUE(m_oauth2_config.is_enabled());
    }

    // Verify undefined 'expires_in' and 'scope'.
    {
        m_scoped.server()->next_request().then([](test_request *request)
        {
            std::map<utility::string_t, utility::string_t> headers;
            headers[header_names::content_type] = mime_types::application_json;
            request->reply(status_codes::OK, U(""), headers, "{\"access_token\":\"123\",\"token_type\":\"bearer\"}");
        });

        const utility::string_t test_scope(U("wally world"));
        m_oauth2_config.set_scope(test_scope);

        m_oauth2_config.token_from_code(U("")).wait();
        VERIFY_ARE_EQUAL(oauth2_token::undefined_expiration, m_oauth2_config.token().expires_in());
        VERIFY_ARE_EQUAL(test_scope, m_oauth2_config.token().scope());
    }
}

} // SUITE(oauth2_tests)

}}}}
