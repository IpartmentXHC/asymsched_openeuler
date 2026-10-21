from flask import Flask, request, jsonify

try:
    from .asymsched import asymsched
    from .models import App, Placement
except ImportError:  # Support running this file from the package directory.
    from asymsched import asymsched
    from models import App, Placement

flask_app = Flask(__name__)

@flask_app.route('/api/asymsched_once', methods=['POST'])
def asymsched_once():
    """ 单次调用asymsched算法，返回最优放置策略
    """

    data = request.json

    assert 'apps' in data
    assert 'bandwidths' in data
    assert 'remote_access' in data

    param_apps = []
    for app_id, app_data in enumerate(data['apps']):
        param_apps.append(App())
        param_apps[app_id].set_data(app_data)

    apps, placements, min_pid, do_migration = asymsched(param_apps, data["bandwidths"], data["remote_access"])
    return jsonify({
        'apps': list(map(App.serialize, apps)),
        'placements' : list(map(Placement.serialize, placements)),
        'min_pid': min_pid,
        'do_migration': do_migration
    })

if __name__ == '__main__':
    flask_app.run(debug=True)
