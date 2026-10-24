import os
import re

# 1. Patch web_server.c
f = "components/web_server/web_server.c"
with open(f, "r") as file:
    content = file.read()

# Fix includes
if "#include \"sdkconfig.h\"" not in content:
    content = content.replace("#include \"web_server.h\"", "#include \"sdkconfig.h\"\n#include \"web_server.h\"")

# Fix macros
content = content.replace("HTTPD_413_PAYLOAD_TOO_LARGE", "HTTPD_413_CONTENT_TOO_LARGE")

# Fix 503 response
# We need to replace `httpd_resp_send_err(req, HTTPD_503_SERVICE_UNAVAILABLE, "Server busy");`
# with `httpd_resp_set_status(req, "503 Service Unavailable"); httpd_resp_sendstr(req, "Server busy"); return ESP_OK;`
content = re.sub(
    r'httpd_resp_send_err\(\s*req,\s*HTTPD_503_SERVICE_UNAVAILABLE,\s*"([^"]+)"\s*\);',
    r'httpd_resp_set_status(req, "503 Service Unavailable");\n        httpd_resp_sendstr(req, "\1");\n        return ESP_OK;',
    content
)

with open(f, "w") as file:
    file.write(content)

# 2. Patch Kconfig.projbuild
f2 = "main/Kconfig.projbuild"
with open(f2, "r") as file:
    content2 = file.read()

content2 = re.sub(r'depends on WEB_SERVER_ENABLE\s*\n\s*default "quad-ipkvm.local"', r'default "quad-ipkvm.local"', content2)
content2 = re.sub(r'depends on WEB_SERVER_ENABLE\s*\n\s*default 10000', r'default 10000', content2)

with open(f2, "w") as file:
    file.write(content2)

# 3. Patch web_cert.c
# Fix the SAN setup from manual extension byte building to standard function calls
# Wait, let's look at what Mbed TLS 4.0 has in IDF 6.0.1.
# It seems `mbedtls_x509write_crt_set_subject_alternative_name` requires an `mbedtls_x509_san_list`.
# Actually wait, `mbedtls_x509write_crt_set_subject_alternative_name` wasn't working earlier either due to struct issues or we didn't try the exact right one.
# Let's fix SAN with the mbedtls_x509_san_list approach
print("Done patching web_server.c and Kconfig.projbuild")
