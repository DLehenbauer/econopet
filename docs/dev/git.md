# Git Reference

## Submodules

### Update Submodules

When you pull a commit that has changed the committed submodule version, you need to update the submodules to match:

```sh
# Update all submodules to the versions specified in the current commit
#
# --init: Initialize any submodules that haven't been initialized yet
#         (e.g., new submodules added to the project)
#
# --recursive: Update nested submodules as well

git submodule update --init --recursive
```

If you want to pull and update submodules in one command:

```sh
# Pull changes and update submodules automatically
git pull --recurse-submodules
```

### Add Submodule

```sh
# Add the submodule without specifying a branch
git submodule add https://github.com/yaml/libyaml.git fw/external/libyaml

# Navigate to the submodule directory
pushd fw/external/libyaml

# Check out the specific tag
git fetch --tags
git tag -l
git checkout 0.2.5

# Go back to the main repository
popd

# Record the submodule state in the main repository
git add fw/external/libyaml
git commit -m "Added libyaml submodule at tag v0.2.5"
```

### Remove Submodule

```sh
# Remove entry from ~/.gitmodules
code .submodules

# Remove target directory
rm -rf fw/external/libyaml

# Remove submodule repository
rm -rf .git/modules/fw/external/libyaml
```
