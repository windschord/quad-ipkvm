import re

f = "components/web_cert/web_cert.c"
with open(f, "r") as file:
    content = file.read()

san_replacement = """
    mbedtls_x509_san_list san_list;
    memset(&san_list, 0, sizeof(san_list));
    san_list.node.type = 2; // MBEDTLS_X509_SAN_DNS_NAME (value 2)
    san_list.node.san.unstructured_name.p = (unsigned char *)"quad-ipkvm.local";
    san_list.node.san.unstructured_name.len = strlen("quad-ipkvm.local");
    san_list.next = NULL;

    if (mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san_list) != 0) {
         ESP_LOGE(TAG, "Failed to set SAN");
         goto err_cleanup;
    }
"""

content = re.sub(r'if \(mbedtls_x509write_crt_set_extension\(&crt, MBEDTLS_OID_SUBJECT_ALT_NAME, MBEDTLS_OID_SIZE\(MBEDTLS_OID_SUBJECT_ALT_NAME\), 0, \(const unsigned char\*\)"\\x82\\x10""quad-ipkvm\.local", 18\) != 0\) \{\s*ESP_LOGE\(TAG, "Failed to set SAN"\);\s*goto err_cleanup;\s*\}', san_replacement.strip(), content)

with open(f, "w") as file:
    file.write(content)

print("Done patching web_cert.c for SAN")
