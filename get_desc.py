with open('pr_description', 'r') as f:
    content = f.read()

import json
print(json.dumps(content))
