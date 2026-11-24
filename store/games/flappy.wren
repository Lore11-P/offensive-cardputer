// name: Flappy
// desc: Passa tra i tubi e non toccarli. ENTER per riprovare.
// version: 1.0
// Flappy per ORBIT OS - 240x135, ENTER per ripartire.
var bg = Game.rgb(10, 22, 40)
var bgDeep = Game.rgb(20, 40, 66)
var panel = Game.rgb(18, 22, 34)
var fg = Game.rgb(225, 238, 255)
var acc = Game.rgb(90, 220, 255)
var hot = Game.rgb(255, 190, 60)
var dim = Game.rgb(110, 125, 150)
var pipeC = Game.rgb(64, 190, 120)
var pipeHi = Game.rgb(120, 230, 160)
var groundC = Game.rgb(96, 74, 52)
var groundHi = Game.rgb(140, 110, 74)
var skyDots = [[22, 24], [58, 33], [96, 21], [140, 30], [176, 20], [214, 34], [30, 100], [118, 104], [204, 96], [72, 108]]

var top = 15
var groundY = 127
var birdX = 52
var birdY = 56
var birdW = 10
var birdH = 8
var vy = 0
var gravity = 0.42
var jump = -4.3
var maxFall = 5.2
var pipeW = 22
var gapSize = 44
var spacing = 96
var speed = 1.55
var state = 0
var score = 0
var pipes = []
var flapLock = false

var addPipe = Fn.new {
  pipes.add([Game.width + 6, 31 + Game.random(45), 0])
}

var newGame = Fn.new {
  birdY = 56
  vy = 0
  score = 0
  pipes = []
  flapLock = true
  state = 1
}

var drawBird = Fn.new { |x, y|
  var wingY = y + 4
  if (Game.millis % 220 < 110) { wingY = y + 2 }
  Game.circle(x + 5, y + 4, 5, hot)
  Game.rect(x + 9, y + 4, 4, 3, Game.rgb(242, 130, 56))
  Game.rect(x + 6, y + 1, 3, 3, fg)
  Game.rect(x + 8, y + 2, 1, 1, bg)
  Game.rect(x + 1, wingY, 4, 3, acc)
}

var drawSky = Fn.new {
  Game.rect(0, 0, Game.width, Game.height, bg)
  for (p in skyDots) {
    Game.rect(p[0], p[1], 2, 2, bgDeep)
  }
}

var drawGround = Fn.new {
  Game.rect(0, groundY, Game.width, Game.height - groundY, groundC)
  Game.rect(0, groundY, Game.width, 1, groundHi)
}

var drawPipes = Fn.new {
  for (p in pipes) {
    Game.rect(p[0], top, pipeW, p[1] - top, pipeC)
    Game.rect(p[0], p[1] - 5, pipeW, 5, pipeHi)
    Game.rect(p[0], p[1] + gapSize, pipeW, groundY - (p[1] + gapSize), pipeC)
    Game.rect(p[0], p[1] + gapSize, pipeW, 5, pipeHi)
  }
}

var drawHeader = Fn.new {
  Game.rect(0, 0, Game.width, top, panel)
  Game.text(6, 4, "FLAPPY", acc, 2)
  var hud = "PUNTI %(score)"
  Game.text(Game.width - 6 - (hud.count * 7 - 2), 3, hud, hot, 2)
}

var drawTitle = Fn.new {
  drawSky.call()
  drawGround.call()
  var title = "FLAPPY"
  Game.text((Game.width - (title.count * 12 - 2)) / 2, 16, title, acc, 4)
  Game.text(69, 38, "Passa tra i tubi.", fg, 0)
  drawBird.call(112, 50)
  Game.text(69, 78, "SU o ENTER: salta", hot, 0)
  Game.text(45, 104, "ENTER: gioca   ESC: esci", dim, 0)
}

var drawGame = Fn.new {
  drawSky.call()
  drawPipes.call()
  drawGround.call()
  drawBird.call(birdX, birdY)
  drawHeader.call()
  if (state == 2) {
    Game.rect(40, 42, 160, 54, panel)
    Game.rect(40, 42, 160, 2, hot)
    var title = "GAME OVER"
    Game.text((Game.width - (title.count * 7 - 2)) / 2, 48, title, hot, 2)
    var s = "PUNTI %(score)"
    Game.text((Game.width - (s.count * 6 - 1)) / 2, 64, s, fg, 0)
    Game.text(45, 80, "ENTER riprova - ESC esci", dim, 0)
  }
}

while (true) {
  Game.frame()
  if (state == 0) {
    drawTitle.call()
    if (Game.select) {
      newGame.call()
    }
  } else {
    if (state == 1) {
      var up = Game.up
      var sel = Game.select
      if ((up || sel) && !flapLock) {
        vy = jump
        flapLock = true
        Game.beep(720, 18)
      }
      if (!up && !sel) {
        flapLock = false
      }
      vy = vy + gravity
      if (vy > maxFall) { vy = maxFall }
      birdY = birdY + vy
      if (birdY < top) {
        birdY = top
        vy = 0
      }
      for (p in pipes) {
        p[0] = p[0] - speed
        if (p[2] == 0 && p[0] + pipeW < birdX) {
          p[2] = 1
          score = score + 1
          Game.beep(980, 20)
        }
      }
      var i = pipes.count - 1
      while (i >= 0) {
        if (pipes[i][0] + pipeW < -2) {
          pipes.removeAt(i)
        }
        i = i - 1
      }
      if (pipes.count == 0 || pipes[pipes.count - 1][0] < Game.width - spacing) {
        addPipe.call()
      }
      var dead = false
      if (birdY + birdH >= groundY) { dead = true }
      for (p in pipes) {
        if (birdX + birdW > p[0] && birdX < p[0] + pipeW) {
          if (birdY < p[1] || birdY + birdH > p[1] + gapSize) {
            dead = true
          }
        }
      }
      if (dead) {
        state = 2
        Game.beep(150, 260)
      }
    } else if (Game.select) {
      newGame.call()
    }
    drawGame.call()
  }
  Game.present()
}
