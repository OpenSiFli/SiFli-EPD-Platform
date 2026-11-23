"""Select a separate SDK output tree for the platform developer build."""


def prepare_environment(building, rtconfig, variant):
    if variant == 'release':
        building.PrepareEnv()
        return

    # The SDK sets OUTPUT_DIR in LoadRtconfig and uses it immediately in
    # PrepareEnv to resolve Kconfig. Child projects inherit the parent output.
    load_board = building.LoadRtconfig

    def load_dev_board(board):
        config = load_board(board)
        config.OUTPUT_DIR = config.OUTPUT_DIR.rstrip('/\\') + '_dev/'
        rtconfig.OUTPUT_DIR = config.OUTPUT_DIR
        return config

    building.LoadRtconfig = load_dev_board
    try:
        building.PrepareEnv()
    finally:
        building.LoadRtconfig = load_board
