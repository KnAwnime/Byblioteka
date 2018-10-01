import importlib
import os
import shutil
import sys
import zipfile

try:
    from requests import get as urlopen
except ImportError:
    if sys.version_info[0] == 2:
        from urllib2 import urlopen  # noqa f811
    else:
        from urllib.request import urlopen

import torch
import torch.utils.model_zoo as model_zoo

KEY_ENTRYPOINTS = 'entrypoints'
KEY_DEPENDENCIES = 'dependencies'
KEY_HELP = 'help_msg'
KEY_MODULE = 'module'
KEY_CHECKPOINT = 'checkpoint'
MASTER_BRANCH = 'master'
ENV_TORCH_HUB_DIR = 'TORCH_HUB_DIR'
DEFAULT_TORCH_HUB_DIR = '~/.torch_hub'
# FUNC_GET_HUB_INFO = 'get_hub_info'
HUB_INFO_KEYS = [KEY_ENTRYPOINTS, KEY_DEPENDENCIES, KEY_HELP]
ENTRY_KEYS = [KEY_MODULE, KEY_CHECKPOINT]


def _git_archive_link(repo, branch):
    return 'https://github.com/' + repo + '/archive/' + branch + '.zip'


def _download_url_to_file(url, filename):
    print('Downloading: \"{}\" to {}'.format(url, filename))
    try:
        response = urlopen(url, stream=True)
        with open(filename, 'wb') as f:
            for data in response.iter_content():
                f.write(data)
    except Exception:
        raise RuntimeError('Failed to download {}'.format(url))


def _load_and_execute_func(module_name, func_name, args=[], kwargs={}):
    # Import the module
    m = importlib.import_module(module_name)
    # Check if callable is defined in the module
    if func_name not in dir(m):
        raise RuntimeError('Cannot find callable {} in module {}'.format(func_name, module_name))
    func = getattr(m, func_name)
    # Check if func is callable
    if not callable(func):
        raise RuntimeError('{} is not callable'.format(func))
    # Call the function
    return func(*args, **kwargs)

def _load_hub_info(module_name):
    # Import the module
    m = importlib.import_module(module_name)
    for key in HUB_INFO_KEYS:
        if key not in dir(m):
            print(dir(m))
            raise RuntimeError('{} are required attributes in hub.py'.format(', '.join(HUB_INFO_KEYS)))
    return getattr(m, KEY_ENTRYPOINTS), getattr(m, KEY_DEPENDENCIES), getattr(m, KEY_HELP)

def check_type(value, T):
    if not isinstance(value, T):
        raise ValueError('Invalid type: {} should be an instance of {}'.format(value, T))
    return value

def _load_single_model(func_name, entrypoints, hub_dir, args, kwargs):
    for e in entrypoints:
        if e[0] == func_name:
            entry = e
            break
        raise RuntimeError('Callable {} not found in hub entrypoints'.format(func_name))

    checkpoint = None
    if len(e) < 2:
        raise ValueError('Invalid entrypoint: func_name and module_name are required fields')
    else:
        module_name = check_type(e[1],str)

    if len(e) > 2:
        checkpoint = check_type(e[2], str)
    if len(e) > 3:
        raise ValueError('too many fields to unpack in entrypoint: we only accept func_name, module_name, checkpoint_url')

    model = _load_and_execute_func(module_name, func_name, args, kwargs)

    if checkpoint is None:
        print('No pretrained weights provided. Proceeding with random initialized weights...')
    else:
        model.load_state_dict(model_zoo.load_url(checkpoint, model_dir=hub_dir, progress=False))
    return model


def load_model(github, model, hub_dir=None, cleanup=False, args=[], kwargs={}):
    r"""
    Load a model from a github repo, with pretrained weights.

    Args:
        github: Required, a string with format "repo_owner/repo_name[:tag_name]" with an optional
            tag/branch. The default branch is `master` if not specified.
            Example: 'ailzhang/torchvision_hub[:master]' # TODO: CHANGE THIS BEFORE MERGE
        model: Required, it can be a single string or a list of strings, where the string is a callable
            and has an entry in the repo's hub entrypoints.
        hub_dir: Optional, local dir to save the intermediate model & checkpoint files.
            If this argument is not specified, env variable `TORCH_HUB_DIR` will be searched first,
            `~/.torch_hub` will be created and used as the fallback.
        cleanup: Optional, whether to delete the intermediate folder after loading the model.
            Default is `False`.

    Returns:
        a single model or a list of models with corresponding pretrained weights.
    """
    # Setup hub_dir to save downloaded files
    if hub_dir is None:
        hub_dir = os.path.expanduser(os.getenv(ENV_TORCH_HUB_DIR, DEFAULT_TORCH_HUB_DIR))
    if not os.path.exists(hub_dir):
        os.makedirs(hub_dir)

    # Parse github repo information
    branch = MASTER_BRANCH
    try:
        if ':' in github:
            repo_info, branch = github.split(':')
        else:
            repo_info = github
        repo_owner, repo_name = repo_info.split('/')
    except Exception:
        raise ValueError('Argument github only accepts format \'repo_owner/repo_name[:branch]\'')

    # Download zipped code from github
    url = _git_archive_link(repo_info, branch)
    cached_file = os.path.join(hub_dir, branch + '.zip')
    _download_url_to_file(url, cached_file)

    extracted_repo = os.path.join(hub_dir, repo_name + '-' + branch)
    repo_dir = os.path.join(hub_dir, repo_name)
    if os.path.exists(extracted_repo):
        shutil.rmtree(extracted_repo)
    if os.path.exists(repo_dir):
        shutil.rmtree(repo_dir)

    # Github uses '{repo_name}-{branch_name}' as folder name which is not importable
    # We need to manually rename it to '{repo_name}'
    # Unzip the code and rename the base folder
    try:
        zipfile.ZipFile(cached_file).extractall(hub_dir)
        os.remove(cached_file)  # remove zip file
        shutil.move(extracted_repo, repo_dir)
        sys.path.insert(0, hub_dir)  # Make Python interpreter aware of the repo
    except Exception:
        raise RuntimeError('Failed to extract/rename the repo')

    # Parse the hub.py in repo to get hub information
    # hub_info = _load_and_execute_func(repo_name + '.hub', FUNC_GET_HUB_INFO)
    entrypoints, dependencies, help_msg = _load_hub_info(repo_name + '.hub')

    # Check dependent packages
    missing_deps = [pkg for pkg in dependencies if importlib.util.find_spec(pkg) is None]
    if len(missing_deps):
        print('Package {} is required from repo author, but missing in your environment.'
              .format(', '.join(missing_deps)))

    # Support loading multiple callables from the same repo at once.
    if isinstance(model, list):
        res = []
        if (args is not None and len(model) != len(args)) or (kwargs is not None and len(kwargs) != len(model)):
            raise ValueError('If not None, args/kwargs should have the same lenght as model')
        for func_name, arg, kwarg in zip(model, args, kwargs):
            res.append(_load_single_model(func_name, entrypoints, hub_dir, arg, kwarg))
    elif isinstance(model, str):
        res = _load_single_model(model, entrypoints, hub_dir, args, kwargs)
    else:
        raise ValueError('Invalid input: model should be a string or a list of strings')

    # Clean up downloaded files and hub_dir
    if cleanup:
        shutil.rmtree(hub_dir)

    return res
